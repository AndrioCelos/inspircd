/*
 * InspIRCd -- Internet Relay Chat Daemon
 *
 *   Copyright (C) 2021 Herman <GermanAizek@yandex.ru>
 *   Copyright (C) 2016 Adam <Adam@anope.org>
 *   Copyright (C) 2014 Mantas Mikulėnas <grawity@gmail.com>
 *   Copyright (C) 2013-2016, 2018 Attila Molnar <attilamolnar@hush.com>
 *   Copyright (C) 2013, 2017-2020 Sadie Powell <sadie@witchery.services>
 *   Copyright (C) 2013 Daniel Vassdal <shutter@canternet.org>
 *   Copyright (C) 2012, 2019 Robby <robby@chatbelgie.be>
 *   Copyright (C) 2009-2010 Daniel De Graaf <danieldg@inspircd.org>
 *   Copyright (C) 2008, 2010 Craig Edwards <brain@inspircd.org>
 *   Copyright (C) 2008 Thomas Stagner <aquanight@inspircd.org>
 *
 * This file is part of InspIRCd.  InspIRCd is free software: you can
 * redistribute it and/or modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation, version 2.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include "inspircd.h"
#include "modules/account.h"
#include "modules/cap.h"
#include "modules/server.h"
#include "modules/ssl.h"

enum
{
	// From IRCv3 sasl-3.1
	RPL_SASLSUCCESS = 903,
	ERR_SASLFAIL = 904,
	ERR_SASLTOOLONG = 905,
	ERR_SASLABORTED = 906,
	RPL_SASLMECHS = 908
};

static std::string sasl_target;

class ServerTracker final
	: public ServerProtocol::LinkEventListener
{
 private:
	bool online;

	void Update(const Server* server, bool linked)
	{
		if (sasl_target == "*")
			return;

		if (InspIRCd::Match(server->GetName(), sasl_target))
		{
			ServerInstance->Logs.Log(MODNAME, LOG_VERBOSE, "SASL target server \"%s\" %s", sasl_target.c_str(), (linked ? "came online" : "went offline"));
			online = linked;
		}
	}

	void OnServerLink(const Server* server) override
	{
		Update(server, true);
	}

	void OnServerSplit(const Server* server, bool error) override
	{
		Update(server, false);
	}

 public:
	ServerTracker(Module* mod)
		: ServerProtocol::LinkEventListener(mod)
	{
		Reset();
	}

	void Reset()
	{
		if (sasl_target == "*")
		{
			online = true;
			return;
		}

		online = false;

		ProtocolInterface::ServerList servers;
		ServerInstance->PI->GetServerList(servers);
		for (const auto& server : servers)
		{
			if (InspIRCd::Match(server.servername, sasl_target))
			{
				online = true;
				break;
			}
		}
	}

	bool IsOnline() const { return online; }
};

class SASLCap final
	: public Cap::Capability
{
 private:
	std::string mechlist;
	const ServerTracker& servertracker;
	UserCertificateAPI sslapi;

	bool OnRequest(LocalUser* user, bool adding) override
	{
		if (requiressl && sslapi && !sslapi->GetCertificate(user))
			return false;

		// Servers MUST NAK any sasl capability request if the authentication layer
		// is unavailable.
		return servertracker.IsOnline();
	}

	bool OnList(LocalUser* user) override
	{
		if (requiressl && sslapi && !sslapi->GetCertificate(user))
			return false;

		// Servers MUST NOT advertise the sasl capability if the authentication layer
		// is unavailable.
		return servertracker.IsOnline();
	}

	const std::string* GetValue(LocalUser* user) const override
	{
		return &mechlist;
	}

 public:
	bool requiressl;
	SASLCap(Module* mod, const ServerTracker& tracker)
		: Cap::Capability(mod, "sasl")
		, servertracker(tracker)
		, sslapi(mod)
	{
	}

	void SetMechlist(const std::string& newmechlist)
	{
		if (mechlist == newmechlist)
			return;

		mechlist = newmechlist;
		NotifyValueChange();
	}
};

enum SaslState { SASL_INIT, SASL_COMM, SASL_DONE };
enum SaslResult { SASL_OK, SASL_FAIL, SASL_ABORT };

static void SendSASL(LocalUser* user, const std::string& agent, char mode, const std::vector<std::string>& parameters)
{
	CommandBase::Params params;
	params.push_back(user->uuid);
	params.push_back(agent);
	params.push_back(ConvToStr(mode));
	params.insert(params.end(), parameters.begin(), parameters.end());
	ServerInstance->PI->SendEncapsulatedData(sasl_target, "SASL", params);
}

static ClientProtocol::EventProvider* g_protoev;

/**
 * Tracks SASL authentication state like charybdis does. --nenolod
 */
class SaslAuthenticator final
{
 private:
	std::string agent;
	LocalUser* user;
	SaslState state = SASL_INIT;
	SaslResult result;
	bool state_announced = false;

	void SendHostIP(UserCertificateAPI& sslapi)
	{
		std::vector<std::string> params;
		params.reserve(3);
		params.push_back(user->GetRealHost());
		params.push_back(user->GetIPString());
		params.push_back(sslapi && sslapi->GetCertificate(user) ? "S" : "P");

		SendSASL(user, "*", 'H', params);
	}

 public:
	SaslAuthenticator(LocalUser* user_, const std::string& method, UserCertificateAPI& sslapi)
		: user(user_)
	{
		SendHostIP(sslapi);

		std::vector<std::string> params;
		params.push_back(method);

		const std::string fp = sslapi ? sslapi->GetFingerprint(user) : "";
		if (!fp.empty())
			params.push_back(fp);

		SendSASL(user, "*", 'S', params);
	}

	SaslResult GetSaslResult(const std::string &result_)
	{
		if (result_ == "F")
			return SASL_FAIL;

		if (result_ == "A")
			return SASL_ABORT;

		return SASL_OK;
	}

	/* checks for and deals with a state change. */
	SaslState ProcessInboundMessage(const CommandBase::Params& msg)
	{
		switch (this->state)
		{
			case SASL_INIT:
				this->agent = msg[0];
				this->state = SASL_COMM;
				[[fallthrough]];

			case SASL_COMM:
				if (msg[0] != this->agent)
					return this->state;

				if (msg.size() < 4)
					return this->state;

				if (msg[2] == "C")
				{
					LocalUser* const localuser = IS_LOCAL(user);
					if (localuser)
					{
						ClientProtocol::Message authmsg("AUTHENTICATE");
						authmsg.PushParamRef(msg[3]);
						ClientProtocol::Event authevent(*g_protoev, authmsg);
						localuser->Send(authevent);
					}
				}
				else if (msg[2] == "D")
				{
					this->state = SASL_DONE;
					this->result = this->GetSaslResult(msg[3]);
				}
				else if (msg[2] == "M")
					this->user->WriteNumeric(RPL_SASLMECHS, msg[3], "are available SASL mechanisms");
				else
					ServerInstance->Logs.Log(MODNAME, LOG_DEFAULT, "Services sent an unknown SASL message \"%s\" \"%s\"", msg[2].c_str(), msg[3].c_str());
				break;

			case SASL_DONE:
				break;
		}

		return this->state;
	}

	bool SendClientMessage(const std::vector<std::string>& parameters)
	{
		if (this->state != SASL_COMM)
			return true;

		SendSASL(this->user, this->agent, 'C', parameters);

		if (parameters[0].c_str()[0] == '*')
		{
			this->state = SASL_DONE;
			this->result = SASL_ABORT;
			return false;
		}

		return true;
	}

	void AnnounceState(void)
	{
		if (this->state_announced)
			return;

		switch (this->result)
		{
		case SASL_OK:
			this->user->WriteNumeric(RPL_SASLSUCCESS, "SASL authentication successful");
			break;
		case SASL_ABORT:
			this->user->WriteNumeric(ERR_SASLABORTED, "SASL authentication aborted");
			break;
		case SASL_FAIL:
			this->user->WriteNumeric(ERR_SASLFAIL, "SASL authentication failed");
			break;
		}

		this->state_announced = true;
	}
};

class CommandAuthenticate final
	: public SplitCommand
{
 private:
	// The maximum length of an AUTHENTICATE request.
	static const size_t MAX_AUTHENTICATE_SIZE = 400;

 public:
	SimpleExtItem<SaslAuthenticator>& authExt;
	Cap::Capability& cap;
	UserCertificateAPI sslapi;

	CommandAuthenticate(Module* Creator, SimpleExtItem<SaslAuthenticator>& ext, Cap::Capability& Cap)
		: SplitCommand(Creator, "AUTHENTICATE", 1)
		, authExt(ext)
		, cap(Cap)
		, sslapi(Creator)
	{
		works_before_reg = true;
		allow_empty_last_param = false;
	}

	CmdResult HandleLocal(LocalUser* user, const Params& parameters) override
	{
		{
			if (!cap.IsEnabled(user))
				return CmdResult::FAILURE;

			if (parameters[0].find(' ') != std::string::npos || parameters[0][0] == ':')
				return CmdResult::FAILURE;

			if (parameters[0].length() > MAX_AUTHENTICATE_SIZE)
			{
				user->WriteNumeric(ERR_SASLTOOLONG, "SASL message too long");
				return CmdResult::FAILURE;
			}

			SaslAuthenticator *sasl = authExt.Get(user);
			if (!sasl)
				authExt.Set(user, user, parameters[0], sslapi);
			else if (sasl->SendClientMessage(parameters) == false)	// IAL abort extension --nenolod
			{
				sasl->AnnounceState();
				authExt.Unset(user);
			}
		}
		return CmdResult::FAILURE;
	}
};

class CommandSASL final
	: public Command
{
 public:
	SimpleExtItem<SaslAuthenticator>& authExt;
	CommandSASL(Module* Creator, SimpleExtItem<SaslAuthenticator>& ext) : Command(Creator, "SASL", 2), authExt(ext)
	{
		this->access_needed = CmdAccess::SERVER; // should not be called by users
	}

	CmdResult Handle(User* user, const Params& parameters) override
	{
		User* target = ServerInstance->Users.FindUUID(parameters[1]);
		if (!target)
		{
			ServerInstance->Logs.Log(MODNAME, LOG_DEBUG, "User not found in sasl ENCAP event: %s", parameters[1].c_str());
			return CmdResult::FAILURE;
		}

		SaslAuthenticator *sasl = authExt.Get(target);
		if (!sasl)
			return CmdResult::FAILURE;

		SaslState state = sasl->ProcessInboundMessage(parameters);
		if (state == SASL_DONE)
		{
			sasl->AnnounceState();
			authExt.Unset(target);
		}
		return CmdResult::SUCCESS;
	}

	RouteDescriptor GetRouting(User* user, const Params& parameters) override
	{
		return ROUTE_BROADCAST;
	}
};

class ModuleSASL final
	: public Module
{
 private:
	SimpleExtItem<SaslAuthenticator> authExt;
	ServerTracker servertracker;
	SASLCap cap;
	CommandAuthenticate auth;
	CommandSASL sasl;
	ClientProtocol::EventProvider protoev;

 public:
	ModuleSASL()
		: Module(VF_VENDOR, "Provides the IRCv3 sasl client capability.")
		, authExt(this, "sasl_auth", ExtensionItem::EXT_USER)
		, servertracker(this)
		, cap(this, servertracker)
		, auth(this, authExt, cap)
		, sasl(this, authExt)
		, protoev(this, auth.name)
	{
		g_protoev = &protoev;
	}

	void init() override
	{
		if (!ServerInstance->Modules.Find("services_account") || !ServerInstance->Modules.Find("cap"))
			ServerInstance->Logs.Log(MODNAME, LOG_DEFAULT, "WARNING: m_services_account and m_cap are not loaded! m_sasl will NOT function correctly until these two modules are loaded!");
	}

	void ReadConfig(ConfigStatus& status) override
	{
		auto tag = ServerInstance->Config->ConfValue("sasl");

		const std::string target = tag->getString("target");
		if (target.empty())
			throw ModuleException("<sasl:target> must be set to the name of your services server!");

		cap.requiressl = tag->getBool("requiressl");
		sasl_target = target;
		servertracker.Reset();
	}

	void OnDecodeMetaData(Extensible* target, const std::string& extname, const std::string& extdata) override
	{
		if ((target == NULL) && (extname == "saslmechlist"))
			cap.SetMechlist(extdata);
	}
};

MODULE_INIT(ModuleSASL)
