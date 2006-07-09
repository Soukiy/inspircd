#ifndef INSPIRCD_LISTMODE_PROVIDER
#define INSPIRCD_LISTMODE_PROVIDER

#include <stdio.h>
#include <string>
#include <sstream>
#include <vector>
#include "users.h"
#include "channels.h"
#include "modules.h"
#include "helperfuncs.h"

/* $ModDesc: Provides support for easily creating listmodes, stores the time set, the user, and a parameter. */

/* Updated to use the <banlist> config tag if it exists */
/* Written by Om <omster@gmail.com>, December 2005. */
/* Based on code previously written by Om - April 2005 */
/* Updated to new API July 8th 2006 by Brain */
/* Originally based on m_chanprotect and m_silence */

inline std::string stringtime()
{
	std::ostringstream TIME;
	TIME << time(NULL); 
	return TIME.str();
}

class ListItem
{
public:
	std::string nick;
	std::string mask;
	std::string time;
};

class ListLimit
{
public:
	std::string mask;
	unsigned int limit;
};

// Just defining the type we use for the exception list here...
typedef std::vector<ListItem> modelist;
typedef std::vector<ListLimit> limitlist;

class ListModeBase : public ModeHandler
{
 protected:
	Server* Srv;
 
	std::string infokey;
	std::string listnumeric;
	std::string endoflistnumeric;
	std::string endofliststring;
 	std::string configtag;
	limitlist chanlimits;
 
 public:
	ListModeBase(Server* serv, char modechar, const std::string &eolstr, const std::string &lnum, const std::string &eolnum, const std::string &ctag = "banlist")
 	: ModeHandler(modechar, 1, 1, true, MODETYPE_CHANNEL, false), Srv(serv), listnumeric(lnum), endoflistnumeric(eolnum), endofliststring(eolstr), configtag(ctag)
	{
		this->DoRehash();
		infokey = "exceptionbase_mode_" + std::string(1, mode) + "_list";
	}

	virtual void DisplayList(userrec* user, chanrec* channel)
	{
		modelist* el = (modelist*)channel->GetExt(infokey);
		if (el)
		{
			for(modelist::iterator it = el->begin(); it != el->end(); it++)
			{
				WriteServ(user->fd, "%s %s %s %s %s %s", listnumeric.c_str(), user->nick, channel->name, it->mask.c_str(), it->nick.c_str(), it->time.c_str());
			}
		}
		WriteServ(user->fd, "%s %s %s %s", endoflistnumeric.c_str(), user->nick, channel->name, endofliststring.c_str());
	}

	virtual void DoRehash()
	{
		ConfigReader Conf;

		chanlimits.clear();

		for(int i = 0; i < Conf.Enumerate(configtag); i++)
		{
			// For each <banlist> tag
			ListLimit limit;
			limit.mask = Conf.ReadValue(configtag, "chan", i);
			limit.limit = Conf.ReadInteger(configtag, "limit", i, true);

			if(limit.mask.size() && limit.limit > 0)
			{
				chanlimits.push_back(limit);
				log(DEBUG, "Read channel listmode limit of %u for mask '%s'", limit.limit, limit.mask.c_str());
			}
			else
			{
				log(DEBUG, "Invalid tag");
			}
		}
		if(chanlimits.size() == 0)
		{
			ListLimit limit;
			limit.mask = "*";
			limit.limit = 64;
			chanlimits.push_back(limit);
		}
	}

	virtual void DoImplements(char* List)
	{
		List[I_OnChannelDelete] = List[I_OnSyncChannel] = List[I_OnCleanup] = List[I_OnRehash] = 1;
	}

	virtual ModeAction OnModeChange(userrec* source, userrec* dest, chanrec* channel, std::string &parameter, bool adding)
	{
		// Try and grab the list
		modelist* el = (modelist*)channel->GetExt(infokey);

		if (adding)
		{
			// If there was no list
			if (!el)
			{
				// Make one
				el = new modelist;
				channel->Extend(infokey, (char*)el);
			}

			// Clean the mask up
			ModeParser::CleanMask(parameter);

			// Check if the item already exists in the list
			for (modelist::iterator it = el->begin(); it != el->end(); it++)
			{
				if(parameter == it->mask)
				{
					// it does, deny the change
					parameter = "";
					return MODEACTION_DENY;
				}
			}

			unsigned int maxsize = 0;

			for (limitlist::iterator it = chanlimits.begin(); it != chanlimits.end(); it++)
			{
				if (Srv->MatchText(channel->name, it->mask))
				{
					// We have a pattern matching the channel...
					maxsize = el->size();
					if (maxsize < it->limit)
					{
						// And now add the mask onto the list...
						ListItem e;
						e.mask = parameter;
						e.nick = source->nick;
						e.time = stringtime();

						el->push_back(e);
						return MODEACTION_ALLOW;
					}
				}
			}

			// List is full
			WriteServ(source->fd, "478 %s %s %s :Channel ban/ignore list is full", source->nick, channel->name, parameter.c_str());
			parameter = "";
			return MODEACTION_DENY;
		}
		else
		{
			// We're taking the mode off
			if (el)
			{
				for (modelist::iterator it = el->begin(); it != el->end(); it++)
				{
					if(parameter == it->mask)
					{
						el->erase(it);
						if(el->size() == 0)
						{
							channel->Shrink(infokey);
							delete el;
						}
						return MODEACTION_ALLOW;
					}
				}
				parameter = "";
				return MODEACTION_DENY;
			}
			else
			{
				// Hmm, taking an exception off a non-existant list, DIE
				parameter = "";
				return MODEACTION_DENY;
			}
		}
		return MODEACTION_DENY;
	}

	virtual std::string& GetInfoKey()
	{
		return infokey;
	}

	virtual void DoChannelDelete(chanrec* chan)
	{
		modelist* list = (modelist*)chan->GetExt(infokey);

		if (list)
		{
			chan->Shrink(infokey);
			delete list;
		}
	}

	virtual void DoSyncChannel(chanrec* chan, Module* proto, void* opaque)
	{
		modelist* list = (modelist*)chan->GetExt(infokey);
		if (list)
		{
			for (modelist::iterator it = list->begin(); it != list->end(); it++)
			{
				proto->ProtoSendMode(opaque, TYPE_CHANNEL, chan, "+" + std::string(1, mode) + " " + it->mask);
			}
		}
	}

	virtual void DoCleanup(int target_type, void* item)
	{
		if (target_type == TYPE_CHANNEL)
		{
			chanrec* chan = (chanrec*)item;

			modelist* list = (modelist*)chan->GetExt(infokey);

			if (list)
			{
				chan->Shrink(infokey);
				delete list;
			}
		}
	}
};

#endif
