/*       +------------------------------------+
 *       | Inspire Internet Relay Chat Daemon |
 *       +------------------------------------+
 *
 *  InspIRCd: (C) 2002-2008 InspIRCd Development Team
 * See: http://www.inspircd.org/wiki/index.php/Credits
 *
 * This program is free but copyrighted software; see
 *            the file COPYING for details.
 *
 * ---------------------------------------------------
 */

#ifndef __THREADENGINE__
#define __THREADENGINE__

#include <vector>
#include <string>
#include <map>
#include "inspircd_config.h"
#include "base.h"

class InspIRCd;

class CoreExport Thread : public Extensible
{
 public:
	Thread() { };
	virtual ~Thread() { };
	virtual void Run() = 0;
};

class CoreExport ThreadEngine : public Extensible
{
 protected:

	 InspIRCd* ServerInstance;
	 Thread* NewThread;

 public:

	ThreadEngine(InspIRCd* Instance);

	virtual ~ThreadEngine();

	virtual bool Mutex(bool enable) = 0;

	virtual void Run() = 0;

	virtual void Create(Thread* thread_to_init) = 0;
};

#endif
