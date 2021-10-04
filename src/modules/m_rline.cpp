/*
 * InspIRCd -- Internet Relay Chat Daemon
 *
 *   Copyright (C) 2018 linuxdaemon <linuxdaemon.irc@gmail.com>
 *   Copyright (C) 2017, 2019 Matt Schatz <genius3000@g3k.solutions>
 *   Copyright (C) 2013, 2017-2018, 2020 Sadie Powell <sadie@witchery.services>
 *   Copyright (C) 2012-2013, 2016 Attila Molnar <attilamolnar@hush.com>
 *   Copyright (C) 2012, 2019 Robby <robby@chatbelgie.be>
 *   Copyright (C) 2009-2010 Daniel De Graaf <danieldg@inspircd.org>
 *   Copyright (C) 2009 Uli Schlachter <psychon@inspircd.org>
 *   Copyright (C) 2008-2009 Robin Burchell <robin+git@viroteck.net>
 *   Copyright (C) 2008 Thomas Stagner <aquanight@inspircd.org>
 *   Copyright (C) 2008 Craig Edwards <brain@inspircd.org>
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
#include "modules/regex.h"
#include "modules/stats.h"
#include "xline.h"

static bool ZlineOnMatch = false;
static bool added_zline = false;

class RLine final
	: public XLine
{
 public:
	RLine(time_t s_time, unsigned long d, const std::string& src, const std::string& re, const std::string& regexs, Regex::EngineReference& rxfactory)
		: XLine(s_time, d, src, re, "R")
		, matchtext(regexs)
	{
		/* This can throw on failure, but if it does we DONT catch it here, we catch it and display it
		 * where the object is created, we might not ALWAYS want it to output stuff to snomask x all the time
		 */
		regex = rxfactory->Create(regexs);
	}

	bool Matches(User* u) override
	{
		LocalUser* lu = IS_LOCAL(u);
		if (lu && lu->exempt)
			return false;

		const std::string host = u->nick + "!" + u->ident + "@" + u->GetRealHost() + " " + u->GetRealName();
		const std::string ip = u->nick + "!" + u->ident + "@" + u->GetIPString() + " " + u->GetRealName();
		return (regex->IsMatch(host) || regex->IsMatch(ip));
	}

	bool Matches(const std::string& compare) override
	{
		return regex->IsMatch(compare);
	}

	void Apply(User* u) override
	{
		if (ZlineOnMatch)
		{
			ZLine* zl = new ZLine(ServerInstance->Time(), duration ? expiry - ServerInstance->Time() : 0, ServerInstance->Config->ServerName.c_str(), reason.c_str(), u->GetIPString());
			if (ServerInstance->XLines->AddLine(zl, NULL))
			{
				std::string expirystr = zl->duration ? InspIRCd::Format(" to expire in %s (on %s)", InspIRCd::DurationString(zl->duration).c_str(), InspIRCd::TimeString(zl->expiry).c_str()) : "";
				ServerInstance->SNO.WriteToSnoMask('x', "Z-line added due to R-line match on %s%s: %s",
					zl->ipaddr.c_str(), expirystr.c_str(), zl->reason.c_str());
				added_zline = true;
			}
			else
				delete zl;
		}
		DefaultApply(u, "R", false);
	}

	const std::string& Displayable() override
	{
		return matchtext;
	}

	std::string matchtext;

	Regex::PatternPtr regex;
};

/** An XLineFactory specialized to generate RLine* pointers
 */
class RLineFactory final
	: public XLineFactory
{
 public:
	Regex::EngineReference& rxfactory;
	RLineFactory(Regex::EngineReference& rx) : XLineFactory("R"), rxfactory(rx)
	{
	}

	/** Generate a RLine
	 */
	XLine* Generate(time_t set_time, unsigned long duration, const std::string& source, const std::string& reason, const std::string& xline_specific_mask) override
	{
		if (!rxfactory)
		{
			ServerInstance->SNO.WriteToSnoMask('a', "Cannot create regexes until engine is set to a loaded provider!");
			throw ModuleException("Regex engine not set or loaded!");
		}

		return new RLine(set_time, duration, source, reason, xline_specific_mask, rxfactory);
	}
};

class CommandRLine final
	: public Command
{
	std::string rxengine;
	RLineFactory& factory;

 public:
	CommandRLine(Module* Creator, RLineFactory& rlf) : Command(Creator,"RLINE", 1, 3), factory(rlf)
	{
		access_needed = CmdAccess::OPERATOR;
		syntax = { "<regex> [<duration> :<reason>]" };
	}

	CmdResult Handle(User* user, const Params& parameters) override
	{

		if (parameters.size() >= 3)
		{
			// Adding - XXX todo make this respect <insane> tag perhaps..

			unsigned long duration;
			if (!InspIRCd::Duration(parameters[1], duration))
			{
				user->WriteNotice("*** Invalid duration for R-line.");
				return CmdResult::FAILURE;
			}
			XLine *r = NULL;

			try
			{
				r = factory.Generate(ServerInstance->Time(), duration, user->nick.c_str(), parameters[2].c_str(), parameters[0].c_str());
			}
			catch (ModuleException &e)
			{
				ServerInstance->SNO.WriteToSnoMask('a', "Could not add R-line: " + e.GetReason());
			}

			if (r)
			{
				if (ServerInstance->XLines->AddLine(r, user))
				{
					if (!duration)
					{
						ServerInstance->SNO.WriteToSnoMask('x', "%s added permanent R-line for %s: %s", user->nick.c_str(), parameters[0].c_str(), parameters[2].c_str());
					}
					else
					{
						ServerInstance->SNO.WriteToSnoMask('x', "%s added timed R-line for %s, expires in %s (on %s): %s",
							user->nick.c_str(), parameters[0].c_str(), InspIRCd::DurationString(duration).c_str(),
							InspIRCd::TimeString(ServerInstance->Time() + duration).c_str(), parameters[2].c_str());
					}

					ServerInstance->XLines->ApplyLines();
				}
				else
				{
					delete r;
					user->WriteNotice("*** R-line for " + parameters[0] + " already exists.");
				}
			}
		}
		else
		{
			std::string reason;

			if (ServerInstance->XLines->DelLine(parameters[0].c_str(), "R", reason, user))
			{
				ServerInstance->SNO.WriteToSnoMask('x', "%s removed R-line on %s: %s", user->nick.c_str(), parameters[0].c_str(), reason.c_str());
			}
			else
			{
				user->WriteNotice("*** R-line " + parameters[0] + " not found on the list.");
			}
		}

		return CmdResult::SUCCESS;
	}
};

class ModuleRLine final
	: public Module
	, public Stats::EventListener
{
 private:
	Regex::EngineReference rxfactory;
	RLineFactory f;
	CommandRLine r;
	bool MatchOnNickChange;
	bool initing = true;
	Regex::Engine* factory;

 public:
	ModuleRLine()
		: Module(VF_VENDOR | VF_COMMON, "Adds the /RLINE command which allows server operators to prevent users matching a nickname!username@hostname+realname regular expression from connecting to the server.")
		, Stats::EventListener(this)
		, rxfactory(this)
		, f(rxfactory)
		, r(this, f)
	{
	}

	void init() override
	{
		ServerInstance->XLines->RegisterFactory(&f);
	}

	~ModuleRLine() override
	{
		ServerInstance->XLines->DelAll("R");
		ServerInstance->XLines->UnregisterFactory(&f);
	}

	void GetLinkData(LinkData& data, std::string& compatdata) override
	{
		if (rxfactory)
			data["regex"] = compatdata = rxfactory->name;
		else
			data["regex"] = "broken";
	}

	ModResult OnUserRegister(LocalUser* user) override
	{
		// Apply lines on user connect
		XLine *rl = ServerInstance->XLines->MatchesLine("R", user);

		if (rl)
		{
			// Bang. :P
			rl->Apply(user);
			return MOD_RES_DENY;
		}
		return MOD_RES_PASSTHRU;
	}

	void ReadConfig(ConfigStatus& status) override
	{
		auto tag = ServerInstance->Config->ConfValue("rline");

		MatchOnNickChange = tag->getBool("matchonnickchange");
		ZlineOnMatch = tag->getBool("zlineonmatch");
		std::string newrxengine = tag->getString("engine");

		factory = rxfactory ? (rxfactory.operator->()) : NULL;

		rxfactory.SetEngine(newrxengine);
		if (!rxfactory)
		{
			if (newrxengine.empty())
				ServerInstance->SNO.WriteToSnoMask('a', "WARNING: No regex engine loaded - R-line functionality disabled until this is corrected.");
			else
				ServerInstance->SNO.WriteToSnoMask('a', "WARNING: Regex engine '%s' is not loaded - R-line functionality disabled until this is corrected.", newrxengine.c_str());

			ServerInstance->XLines->DelAll(f.GetType());
		}
		else if ((!initing) && (rxfactory.operator->() != factory))
		{
			ServerInstance->SNO.WriteToSnoMask('a', "Regex engine has changed, removing all R-lines.");
			ServerInstance->XLines->DelAll(f.GetType());
		}

		initing = false;
	}

	ModResult OnStats(Stats::Context& stats) override
	{
		if (stats.GetSymbol() != 'R')
			return MOD_RES_PASSTHRU;

		ServerInstance->XLines->InvokeStats("R", stats);
		return MOD_RES_DENY;
	}

	void OnUserPostNick(User *user, const std::string &oldnick) override
	{
		if (!IS_LOCAL(user))
			return;

		if (!MatchOnNickChange)
			return;

		XLine *rl = ServerInstance->XLines->MatchesLine("R", user);

		if (rl)
		{
			// Bang! :D
			rl->Apply(user);
		}
	}

	void OnBackgroundTimer(time_t curtime) override
	{
		if (added_zline)
		{
			added_zline = false;
			ServerInstance->XLines->ApplyLines();
		}
	}

	void OnUnloadModule(Module* mod) override
	{
		// If the regex engine became unavailable or has changed, remove all R-lines.
		if (!rxfactory)
		{
			ServerInstance->XLines->DelAll(f.GetType());
		}
		else if (rxfactory.operator->() != factory)
		{
			factory = rxfactory.operator->();
			ServerInstance->XLines->DelAll(f.GetType());
		}
	}

	void Prioritize() override
	{
		Module* mod = ServerInstance->Modules.Find("gateway");
		ServerInstance->Modules.SetPriority(this, I_OnUserRegister, PRIORITY_AFTER, mod);
	}
};

MODULE_INIT(ModuleRLine)
