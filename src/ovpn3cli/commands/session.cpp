//  OpenVPN 3 Linux client -- Next generation OpenVPN client
//
//  Copyright (C) 2018 - 2019  OpenVPN, Inc. <sales@openvpn.net>
//  Copyright (C) 2018 - 2019  David Sommerseth <davids@openvpn.net>
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU Affero General Public License as
//  published by the Free Software Foundation, version 3 of the
//  License.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU Affero General Public License for more details.
//
//  You should have received a copy of the GNU Affero General Public License
//  along with this program.  If not, see <https://www.gnu.org/licenses/>.
//

/**
 * @file   session.hpp
 *
 * @brief  Commands to start and manage VPN sessions
 */

#include <json/json.h>

#include "dbus/core.hpp"
#include "common/cmdargparser.hpp"
#include "common/lookup.hpp"
#include "common/requiresqueue.hpp"
#include "configmgr/proxy-configmgr.hpp"
#include "sessionmgr/proxy-sessionmgr.hpp"
#include "../arghelpers.hpp"

/**
 *  Fetches all the gathered statistics for a specific session
 *
 * @param session_path  std::string containing the D-Bus session path
 * @return Returns the statistics as the ConnectionStats type.
 */
static ConnectionStats fetch_stats(std::string session_path)
{
    try
    {
        OpenVPN3SessionProxy session(G_BUS_TYPE_SYSTEM, session_path);
        if (!session.CheckObjectExists())
        {
            throw CommandException("session-stats",
                                   "Session not found");
        }
        return session.GetConnectionStats();
    }
    catch (DBusException& err)
    {
        std::stringstream errmsg;
        errmsg << "Failed to fetch statistics: " << err.GetRawError();
        throw CommandException("session-stats",  errmsg.str());
    }
}


/**
 *  Converts ConnectionStats into a plain-text string
 *
 * @param stats  The ConnectionStats object returned by fetch_stats()
 * @return Returns std::string with the statistics pre-formatted as text/plain
 */
std::string statistics_plain(ConnectionStats& stats)
{
    if (stats.size() < 1)
    {
        return "";
    }

    std::stringstream out;
    out << std::endl << "Connection statistics:" << std::endl;
    for (auto& sd : stats)
    {
        out << "     "
            << sd.key
            << std::setw(20-sd.key.size()) << std::setfill('.') << "."
            << std::setw(12) << std::setfill('.')
            << sd.value
            << std::endl;
    }
    out << std::endl;
    return out.str();
}


/**
 *  Similiar to statistics_plain(), but returns a JSON string blob with the
 *  statistics data
 *
 * @param stats  The ConnectionStats object returned by fetch_stats()
 * @return Returns std::string with the statistics pre-formatted as JSON
 *  */
static std::string statistics_json(ConnectionStats& stats)
{
    Json::Value outdata;

    for (auto& sd : stats)
    {
        outdata[sd.key] = (Json::Value::Int64) sd.value;
    }
    std::stringstream res;
    res << outdata;
    res << std::endl;
    return res.str();
}


/**
 *  openvpn3 session-stats command
 *
 *  Dumps statistics for a specific VPN session in either text/plain or JSON
 *
 * @param args  ParsedArgs object containing all related options and arguments
 * @return Returns the exit code which will be returned to the calling shell
 */
static int cmd_session_stats(ParsedArgs args)
{
    if (!args.Present("path") && !args.Present("config")
        && !(args.Present("interface")))
    {
        throw CommandException("session-stats",
                               "Missing required session path, config "
                               "or interface name");
    }

    try
    {
        OpenVPN3SessionProxy sessmgr(G_BUS_TYPE_SYSTEM,
                                     OpenVPN3DBus_rootp_sessions);
        std::string sesspath = "";
        if (args.Present("config"))
        {
            std::vector<std::string> paths = sessmgr.LookupConfigName(args.GetValue("config", 0));
            if (0 == paths.size())
            {
                throw CommandException("session-manage",
                                       "No sessions started with the "
                                       "configuration profile name was found");
            }
            else if (1 < paths.size())
            {
                throw CommandException("session-manage",
                                       "More than one session with the given "
                                       "configuration profile name was found.");
            }
            sesspath = paths.at(0);
        }
        else if (args.Present("interface"))
        {
            try
            {
                sesspath = sessmgr.LookupInterface(args.GetValue("interface", 0));
            }
            catch (DBusException& excp)
            {
                throw CommandException("session-stats", excp.GetRawError());
            }
        }
        else
        {
            sesspath = args.GetValue("path", 0);
        }

        ConnectionStats stats = fetch_stats(sesspath);

        std::cout << (args.Present("json") ? statistics_json(stats)
                                           : statistics_plain(stats));
        return 0;
    }
    catch (...)
    {
        throw;
    }
}

/**
 *  Creates the SingleCommand object for the 'session-stats' command
 *
 * @return  Returns a SingleCommand::Ptr object declaring the command
 */
SingleCommand::Ptr prepare_command_session_stats()
{
    //
    //  session-stats command
    //
    SingleCommand::Ptr cmd;
    cmd.reset(new SingleCommand("session-stats",
                                "Show session statistics",
                                cmd_session_stats));
    auto path_opt = cmd->AddOption("path", 'o', "SESSION-PATH", true,
                                   "Path to the configuration in the "
                                   "configuration manager",
                                   arghelper_session_paths);
    path_opt->SetAlias("session-path");
    cmd->AddOption("config", 'c', "CONFIG-NAME", true,
                   "Alternative to --path, where configuration profile name "
                   "is used instead",
                   arghelper_config_names_sessions);
    cmd->AddOption("interface", 'I', "INTERFACE", true,
                   "Alternative to --path, where tun interface name is used "
                   "instead",
                   arghelper_managed_interfaces);
    cmd->AddOption("json", 'j', "Dump the configuration in JSON format");

    return cmd;
}



//////////////////////////////////////////////////////////////////////////


// Implemented in config.cpp
std::string import_config(const std::string filename,
                          const std::string cfgname,
                          const bool single_use,
                          const bool persistent);

/**
 *  openvpn3 session-start command
 *
 *  This command is used to initate and start a new VPN session
 *
 * @param args
 * @return
 */
static int cmd_session_start(ParsedArgs args)
{
    if (!args.Present("config-path") && !args.Present("config"))
    {
        throw CommandException("session-start",
                               "Either --config or --config-path must be provided");
    }
    if (args.Present("config-path") && args.Present("config"))
    {
        throw CommandException("session-start",
                               "--config and --config-path cannot be used together");
    }
    if (args.Present("persist-tun") && !args.Present("config"))
    {
        throw CommandException("session-start",
                               "--persist-tun can only be used with --config");
    }

    try
    {
        OpenVPN3SessionProxy sessmgr(G_BUS_TYPE_SYSTEM,
                                     OpenVPN3DBus_rootp_sessions);
        sessmgr.Ping();

        std::string cfgpath;
        if (args.Present("config"))
        {
            // This will first lookup the configuration name in the
            // Configuration Manager before attempting to load a local file.
            // If multiple configurations carry the same configuration name,
            // it will fail as we do not support handling this scenario.  It

            std::string cfgname = args.GetValue("config", 0);
            try
            {
                cfgpath = retrieve_config_path("session-start", cfgname);
                std::cout << "Using pre-loaded configuration profile '"
                          << cfgname << "'" << std::endl;
            }
            catch (const CommandException& excp)
            {
                std::string err(excp.what());
                if (err.find("More than one configuration profile was found") != std::string::npos)
                {
                    throw;
                }
                cfgpath = import_config(cfgname, cfgname, true, false);
                std::cout << "Using configuration profile from file: "
                          << cfgname << std::endl;
            }
        }
        else
        {
            cfgpath = args.GetValue("config-path", 0);
        }

        // If --persist-tun is given on the command line, enforce this
        // feature on this connection.  This can only be provided when using
        // --config, not --config-path.
        if (args.Present("persist-tun"))
        {
            OpenVPN3ConfigurationProxy cfgprx(G_BUS_TYPE_SYSTEM, cfgpath);
            const ValidOverride& vo = cfgprx.LookupOverride("persist-tun");
            cfgprx.SetOverride(vo, true);
        }

        std::string sessionpath = sessmgr.NewTunnel(cfgpath);

        sleep(1);  // Allow session to be established (FIXME: Signals?)
        std::cout << "Session path: " << sessionpath << std::endl;
        OpenVPN3SessionProxy session(G_BUS_TYPE_SYSTEM, sessionpath);

        unsigned int loops = 10;
        while (loops > 0)
        {
            loops--;
            try
            {
                session.Ready();  // If not, an exception will be thrown
                session.Connect();

                // Allow approx 30 seconds to establish connection; one loop
                // will take about 1.3 seconds.
                unsigned int attempts = 23;
                StatusEvent s;
                while (attempts > 0)
                {
                    attempts--;
                    usleep(300000);  // sleep 0.3 seconds - avg setup time
                    try
                    {
                        s = session.GetLastStatus();
                    }
                    catch (DBusException& excp)
                    {
                        std::string err(excp.what());
                        if (err.find("Failed retrieving property value for 'status'") != std::string::npos)
                        {
                            throw CommandException("session-start",
                                                   "Failed to start session");
                        }
                        throw;
                    }
                    if (s.minor == StatusMinor::CONN_CONNECTED)
                    {
                        std::cout << "Connected" << std::endl;
                        return 0;
                    }
                    else if (s.minor == StatusMinor::CONN_DISCONNECTED
                            || s.minor == StatusMinor::CONN_AUTH_FAILED)
                    {
                        attempts = 0;
                        break;
                    }
                    else if (s.minor == StatusMinor::CFG_REQUIRE_USER)
                    {
                        break;
                    }
                    sleep(1);  // If not yet connected, wait for 1 second
                }

                if (attempts < 1)
                {
                    // FIXME: Look into using exceptions here, catch more
                    // fine grained connection issues from the backend
                    std::cout << "Failed to connect: " << s << std::endl;
                    session.Disconnect();
                    return 3;
                }
            }
            catch (ReadyException& err)
            {
                // If the ReadyException is thrown, it means the backend
                // needs more from the front-end side
                for (auto& type_group : session.QueueCheckTypeGroup())
                {
                    ClientAttentionType type;
                    ClientAttentionGroup group;
                    std::tie(type, group) = type_group;

                    if (ClientAttentionType::CREDENTIALS == type)
                    {
                        std::vector<struct RequiresSlot> reqslots;
                        session.QueueFetchAll(reqslots, type, group);
                        for (auto& r : reqslots)
                        {
                            std::string response;
                            if (!r.hidden_input)
                            {
                                std::cout << r.user_description << ": ";
                                std::cin >> response;
                            }
                            else
                            {
                                std::string prompt = r.user_description + ": ";
                                char *pass = getpass(prompt.c_str());
                                response = std::string(pass);
                            }
                            r.value = response;
                            session.ProvideResponse(r);
                        }
                    }
                }
            }
            catch (DBusException& err)
            {
                std::stringstream errm;
                errm << "Failed to start new session: "
                     << err.GetRawError();
                throw CommandException("session-start", errm.str());
            }
        }

        return 0;
    }
    catch (...)
    {
        throw;
    }
}

/**
 *  Creates the SingleCommand object for the 'session-start' command
 *
 * @return  Returns a SingleCommand::Ptr object declaring the command
 */
SingleCommand::Ptr prepare_command_session_start()
{
    //
    //  session-start command
    //
    SingleCommand::Ptr cmd;
    cmd.reset(new SingleCommand("session-start",
                                "Start a new VPN session",
                                cmd_session_start));
    cmd->AddOption("config", 'c', "CONFIG-FILE", true,
                   "Configuration file to start directly",
                   arghelper_config_names);
    cmd->AddOption("config-path", 'p', "CONFIG-PATH", true,
                   "Configuration path to an already imported configuration",
                   arghelper_config_paths);
    cmd->AddOption("persist-tun", 0,
                   "Enforces persistent tun/seamless tunnel (requires --config)");

    return cmd;
}



//////////////////////////////////////////////////////////////////////////



/**
 *  openvpn3 sessions-list command
 *
 *  Lists all available VPN sessions.  Only sessions where the
 *  calling user is the owner, have been added to the access control list
 *  or sessions tagged with public_access will be listed.  This restriction
 *  is handled by the Session Manager
 *
 * @param args  ParsedArgs object containing all related options and arguments
 * @return Returns the exit code which will be returned to the calling shell
 */
static int cmd_sessions_list(ParsedArgs args)
{
    OpenVPN3SessionProxy sessmgr(G_BUS_TYPE_SYSTEM,
                                 OpenVPN3DBus_rootp_sessions);
    sessmgr.Ping();

    bool first = true;
    for (auto& sessp : sessmgr.FetchAvailableSessions())
    {
        if (sessp.empty())
        {
            continue;
        }
        OpenVPN3SessionProxy sprx(G_BUS_TYPE_SYSTEM, sessp);

        if (first)
        {
            std::cout << std::setw(77) << std::setfill('-') << "-" << std::endl;
        }
        else
        {
            std::cout << std::endl;
        }
        first = false;

        std::string owner;
        pid_t be_pid;
        try
        {
            owner = lookup_username(sprx.GetUIntProperty("owner"));
            be_pid = sprx.GetUIntProperty("backend_pid");
        }
        catch (DBusException&)
        {
            owner = "(not available)";
            be_pid = -1;
        }

        StatusEvent status;
        std::string cfgname_current = "";
        bool config_deleted = false;
        try
        {
            status = sprx.GetLastStatus();
            std::string config_path = sprx.GetStringProperty("config_path");
            try
            {
                OpenVPN3ConfigurationProxy cprx(G_BUS_TYPE_SYSTEM, config_path);
                if (cprx.CheckObjectExists(100))
                {
                    cfgname_current = cprx.GetStringProperty("name");
                }
                else
                {
                    config_deleted = true;
                }
            }
            catch (...)
            {
                // Failure is okay here, the profile may be deleted.
            }
        }
        catch (DBusException &excp)
        {
        }

        std::cout << "        Path: " << sessp << std::endl;

        std::string created;
        try
        {
            std::time_t sess_created = sprx.GetUInt64Property("session_created");
            created = std::asctime(std::localtime(&sess_created));
        }
        catch (DBusException&)
        {
            std::cout << "(Not available)";
        }
        std::cout << "     Created: " << created.substr(0, created.size()-1)
                  << std::setw(47 - created.size()) << std::setfill(' ')
                  << " PID: "
                  << (be_pid > 0 ? std::to_string(be_pid) : "(not available)")
                  << std::endl;


        std::string devname = sprx.GetDeviceName();
        std::cout << "       Owner: " << owner
                  << std::setw(44 - devname.size()) << "Device: " << devname
                  << std::endl;

        std::string cfgname = sprx.GetStringProperty("config_name");
        if (!cfgname.empty())
        {
            std::cout << " Config name: " << cfgname;
            if (config_deleted)
            {
                std::cout << "  (Config not available)";
            }
            else if (cfgname_current != cfgname)
            {
                std::cout << "  (Current name: "
                          << cfgname_current << ")";
            }
            std::cout << std::endl;
        }

        try
        {
            std::string sessname = sprx.GetStringProperty("session_name");
            if (!sessname.empty())
            {
                std::cout << "Session name: " << sessname << std::endl;
            }
        }
        catch (DBusException&)
        {
            // Ignore any errors if this property is unavailable
        }

        std::cout << "      Status: " << status << std::endl;
    }
    if (first)
    {
        std::cout << "No sessions available" << std::endl;
    }
    else
    {
        std::cout << std::setw(77) << std::setfill('-') << "-" << std::endl;
    }

    return 0;
}

/**
 *  Creates the SingleCommand object for the 'session-list' command
 *
 * @return  Returns a SingleCommand::Ptr object declaring the command
 */
SingleCommand::Ptr prepare_command_sessions_list()
{
    //
    //  sessions-list command
    //
    SingleCommand::Ptr cmd;
    cmd.reset(new SingleCommand("sessions-list",
                                "List available VPN sessions",
                                cmd_sessions_list));

    return cmd;
}



//////////////////////////////////////////////////////////////////////////



/**
 *  openvpn3 session-manage command
 *
 *  This command handles pausing, resuming and disconnecting established
 *  sessions.
 *
 * @param args  ParsedArgs object containing all related options and arguments
 * @return Returns the exit code which will be returned to the calling shell
 */
static int cmd_session_manage(ParsedArgs args)
{
    const unsigned int mode_pause      = 1 << 0;
    const unsigned int mode_resume     = 1 << 1;
    const unsigned int mode_restart    = 1 << 2;
    const unsigned int mode_disconnect = 1 << 3;
    const unsigned int mode_cleanup    = 1 << 4;

    unsigned int mode = 0;
    unsigned int mode_count = 0;
    if (args.Present("pause"))
    {
        mode |= mode_pause;
        mode_count++;
    }
    if (args.Present("resume"))
    {
        mode |= mode_resume;
        mode_count++;
    }
    if (args.Present("restart"))
    {
        mode |= mode_restart;
        mode_count++;
    }
    if (args.Present("disconnect"))
    {
        mode |= mode_disconnect;
        mode_count++;
    }
    if (args.Present("cleanup"))
    {
        mode |= mode_cleanup;
        mode_count++;
    }

    if (0 == mode_count)
    {
        throw CommandException("session-manage",
                               "One of --pause, --resume, --restart, "
                               "--disconnect or --cleanup must be present");
    }
    if (1 < mode_count)
    {
        throw CommandException("session-manage",
                               "--pause, --resume, --restart or --disconnect "
                               "cannot be used together");
    }

    // Only --cleanup does NOT depend on --path or --config
    if (!args.Present("path") && !args.Present("config")
        && !args.Present("interface") && (mode ^ mode_cleanup) > 0)
    {
        throw CommandException("session-manage",
                               "Missing required session path or config name");
    }


    try
    {
        OpenVPN3SessionProxy sessmgr(G_BUS_TYPE_SYSTEM,
                                     OpenVPN3DBus_rootp_sessions);

        if (mode_cleanup == mode)
        {
            // Loop through all open sessions and check if they have a valid
            // status available.  A valid status means it is not empty nor
            // unset.  If the status can't be retrieved, it is also
            // invalid.
            std::vector<std::string> sesspaths = sessmgr.FetchAvailableSessions();
            std::cout << "Cleaning up stale sessions - Found "
                      << std::to_string(sesspaths.size()) << " open sessions "
                      << "to check" << std::endl;

            unsigned int c = 0;
            for (const auto &sp : sesspaths)
            {
                OpenVPN3SessionProxy s(G_BUS_TYPE_SYSTEM, sp);
                try
                {
                    std::string cfgname = s.GetStringProperty("config_name");

                    std::cout << "Checking:  " << cfgname << " - " << sp
                              << " ... ";

                    StatusEvent st = s.GetLastStatus();
                    if (st.Check(StatusMajor::UNSET, StatusMinor::UNSET)
                        && st.message.empty())
                    {
                        // This is an empty and unset status
                        // These are rare, as it means the
                        // openvpn3-service-client process is running and
                        // responsive, but has not even managed to load a
                        // configuration profile
                        s.Disconnect();
                        std::cout << "Removed" << std::endl;
                        ++c;
                    }
                    else
                    {
                        std::cout << "Valid, keeping it" << std::endl;
                    }
                }
                catch (const std::exception& e )
                {
                    // Errors in this case indicates we cannot retrieve any
                    // information about the session; thus it is most likely
                    // not valid any more, so we kill it - and ignore any
                    // errors while killing it.
                    //
                    // When this happens the openvpn3-service-client process
                    // is most likely already dead.  Calling the Disconnect()
                    // method basically just cleans up the session in the
                    // Session Manager only.
                    try
                    {
                        s.Disconnect();
                    }
                    catch (...)
                    {
                    }
                    std::cout << "Removed" << std::endl;
                    ++c;
                }
            }
            std::cout << std::to_string(c) << " session" << (c != 1 ? "s" : "")
                      << " removed" << std::endl;
            return 0;
        }


        std::string sesspath = "";
        if (args.Present("config"))
        {
            std::vector<std::string> paths = sessmgr.LookupConfigName(args.GetValue("config", 0));
            if (0 == paths.size())
            {
                throw CommandException("session-manage",
                                       "No sessions started with the "
                                       "configuration profile name was found");
            }
            else if (1 < paths.size())
            {
                throw CommandException("session-manage",
                                       "More than one session with the given "
                                       "configuration profile name was found.");
            }
            sesspath = paths.at(0);
        }
        else if (args.Present("interface"))
        {
            sesspath = sessmgr.LookupInterface(args.GetValue("interface", 0));
        }
        else
        {
            sesspath = args.GetValue("path", 0);
        }

        OpenVPN3SessionProxy session(G_BUS_TYPE_SYSTEM, sesspath);
        if (!session.CheckObjectExists())
        {
            throw CommandException("session-manage",
                                   "Session not found");
        }

        ConnectionStats stats;

        switch (mode)
        {
        case mode_pause:
            session.Pause("Front-end request");
            std::cout << "Initiated session pause: " << sesspath
                      << std::endl;
            return 0;

        case mode_resume:
            session.Resume();
            std::cout << "Initiated session resume: " << sesspath
                      << std::endl;
            return 0;

        case mode_restart:
            session.Restart();
            std::cout << "Initiated session restart: " << sesspath
                      << std::endl;
            return 0;

        case mode_disconnect:
            try
            {
                stats = session.GetConnectionStats();
            }
            catch (...)
            {
                std::cout << "Connection statistics is not available"
                          << std::endl;
            }
            session.Disconnect();
            std::cout << "Initiated session shutdown." << std::endl;
            std::cout << statistics_plain(stats);
            break;
        }
        return 0;
    }
    catch (...)
    {
        throw;
    }
}

/**
 *  Creates the SingleCommand object for the 'session-manage' command
 *
 * @return  Returns a SingleCommand::Ptr object declaring the command
 */
SingleCommand::Ptr prepare_command_session_manage()
{
    //
    //  session-manage command
    //
    SingleCommand::Ptr cmd;
    cmd.reset(new SingleCommand("session-manage",
                                "Manage VPN sessions",
                                cmd_session_manage));
    auto path_opt = cmd->AddOption("path", 'o', "SESSION-PATH", true,
                                   "Path to the session in the session "
                                   "manager",
                                   arghelper_session_paths);
    path_opt->SetAlias("session-path");
    cmd->AddOption("config", 'c', "CONFIG-NAME", true,
                   "Alternative to --path, where configuration profile name "
                   "is used instead",
                   arghelper_config_names_sessions);
    cmd->AddOption("interface", 'I', "INTERFACE", true,
                   "Alternative to --path, where tun interface name is used "
                   "instead",
                   arghelper_managed_interfaces);
    cmd->AddOption("pause", 'P', "Pauses the VPN session");
    cmd->AddOption("resume", 'R', "Resumes a paused VPN session");
    cmd->AddOption("restart", "Disconnect and reconnect a running VPN session");
    cmd->AddOption("disconnect", 'D', "Disconnects a VPN session");
    cmd->AddOption("cleanup", 0, "Clean up stale sessions");

    return cmd;
}



//////////////////////////////////////////////////////////////////////////



/**
 *  openvpn3 session-acl command
 *
 *  Command to modify the access control to a specific VPN session.
 *  All operations related to granting, revoking, public-access are handled
 *  by this command.
 *
 *  Also note that you can run multiple operarations in a single command line.
 *  It is fully possible to use --grant, --revoke and --show in a single
 *  command line.  In addition, both --grant and --revoke can be used multiple
 *  times to grant/revoke several users in a single operation.
 *
 * @param args  ParsedArgs object containing all related options and arguments
 * @return Returns the exit code which will be returned to the calling shell
 *
 */
static int cmd_session_acl(ParsedArgs args)
{
    int ret = 0;
    if (!args.Present("path") && !args.Present("config")
        && !args.Present("interface"))
    {
        throw CommandException("session-acl",
                               "Missing required session path or config name");
    }

    if (!args.Present("show")
        && !args.Present("grant")
        && !args.Present("revoke")
        && !args.Present("public-access")
        && !args.Present("allow-log-access")
        && !args.Present("lock-down")
        && !args.Present("seal"))
    {
        throw CommandException("session-acl", "No operation option provided");
    }

    try
    {
        OpenVPN3SessionProxy sessmgr(G_BUS_TYPE_SYSTEM,
                                     OpenVPN3DBus_rootp_sessions);

        std::string sesspath = "";
        if (args.Present("config"))
        {
            std::vector<std::string> paths = sessmgr.LookupConfigName(args.GetValue("config", 0));
            if (0 == paths.size())
            {
                throw CommandException("session-acl",
                                       "No sessions started with the "
                                       "configuration profile name was found");
            }
            else if (1 < paths.size())
            {
                throw CommandException("session-acl",
                                       "More than one session with the given "
                                       "configuration profile name was found.");
            }
            sesspath = paths.at(0);
        }
        else if (args.Present("interface"))
        {
            sesspath = sessmgr.LookupInterface(args.GetValue("interface", 0));
        }
        else
        {
            sesspath = args.GetValue("path", 0);
        }

        OpenVPN3SessionProxy session(G_BUS_TYPE_SYSTEM, sesspath);
        if (!session.CheckObjectExists())
        {
            throw CommandException("session-acl",
                                   "Session not found");
        }

        if (args.Present("grant"))
        {
            for (auto const& user : args.GetAllValues("grant"))
            {
                uid_t uid = get_userid(user);
                // If uid == -1, something is wrong
                if (-1 == uid)
                {
                    std::cerr << "** ERROR ** --grant " << user
                              << " does not map to a valid user account"
                              << std::endl;
                }
                else
                {
                    // We have a UID ... now grant the access
                    try
                    {
                        session.AccessGrant(uid);
                        std::cout << "Granted access to "
                                  << lookup_username(uid)
                                  << " (uid " << uid << ")"
                                  << std::endl;
                    }
                    catch (DBusException& e)
                    {
                        std::cerr << "Failed granting access to "
                                  << lookup_username(uid)
                                  << " (uid " << uid << ")"
                                  << std::endl;
                        ret = 3;
                    }
                }
            }
        }

        if (args.Present("revoke"))
        {
            for (auto const& user : args.GetAllValues("revoke"))
            {
                uid_t uid = get_userid(user);
                // If uid == -1, something is wrong
                if (-1 == uid)
                {
                    std::cerr << "** ERROR ** --grant " << user
                              << " does not map to a valid user account"
                              << std::endl;
                }
                else {
                    // We have a UID ... now grant the access
                    try
                    {
                        session.AccessRevoke(uid);
                        std::cout << "Access revoked from "
                                  << lookup_username(uid)
                                  << " (uid " << uid << ")"
                                  << std::endl;
                    }
                    catch (DBusException& e)
                    {
                        std::cerr << "Failed revoking access from "
                                  << lookup_username(uid)
                                  << " (uid " << uid << ")"
                                  << std::endl;
                        ret = 3;
                    }

                }
            }
        }
        if (args.Present("public-access"))
        {
            std::string ld = args.GetValue("public-access", 0);
            if (("false" == ld) || ("true" == ld ))
            {
                session.SetPublicAccess("true" == ld);
                if ("true" == ld)
                {
                    std::cout << "Session is now accessible by everyone"
                              << std::endl;
                }
                else
                {
                    std::cout << "Session is now only accessible to "
                              << "specific users"
                              << std::endl;

                }
            }
            else
            {
                std::cerr << "** ERROR ** --public-access argument must be "
                          << "'true' or 'false'"
                          << std::endl;
                ret = 3;
            }
        }

        if (args.Present("allow-log-access"))
        {
            bool ala = args.GetBoolValue("allow-log-access", 0);
            session.SetRestrictLogAccess(!ala);
            if (ala)
            {
                std::cout << "Session log is now accessible to users granted "
                          << " session access" << std::endl;
            }
            else
            {
                std::cout << "Session log is only accessible the session"
                          << " owner" << std::endl;
            }
        }


        if (args.Present("show"))
        {
            std::string owner = lookup_username(session.GetOwner());
            std::cout << "                    Owner: ("
                      << session.GetOwner() << ") "
                      << " " << ('(' != owner[0] ? owner : "(unknown)")
                      << std::endl;

            std::cout << "            Public access: "
                      << (session.GetPublicAccess() ? "yes" : "no")
                      << std::endl;

            std::cout << " Users granted log access: "
                      << (session.GetRestrictLogAccess() ? "no" : "yes")
                      << std::endl;

            if (!session.GetPublicAccess())
            {
                std::vector<uid_t> acl = session.GetAccessList();
                std::cout << "     Users granted access: " << std::to_string(acl.size())
                          << (1 != acl.size() ? " users" : " user")
                          << std::endl;
                for (auto const& uid : acl)
                {
                    std::string user = lookup_username(uid);
                    std::cout << "                           - (" << uid << ") "
                              << " " << ('(' != user[0] ? user : "(unknown)")
                    << std::endl;
                }
            }
        }
        return ret;
    }
    catch (...)
    {
        throw;
    }
}


/**
 *  Creates the SingleCommand object for the 'session-acl' command
 *
 * @return  Returns a SingleCommand::Ptr object declaring the command
 */
SingleCommand::Ptr prepare_command_session_acl()
{
    //
    //  session-acl command
    //
    SingleCommand::Ptr cmd;
    cmd.reset(new SingleCommand("session-acl",
                                "Manage access control lists for sessions",
                                cmd_session_acl));
    auto path_opt = cmd->AddOption("path", 'o', "SESSION-PATH", true,
                                   "Path to the session in the session "
                                   "manager",
                                   arghelper_session_paths);
    path_opt->SetAlias("session-path");
    cmd->AddOption("config", 'c', "CONFIG-NAME", true,
                   "Alternative to --path, where configuration profile name "
                   "is used instead",
                   arghelper_config_names_sessions);
    cmd->AddOption("interface", 'I', "INTERFACE", true,
                   "Alternative to --path, where tun interface name is used "
                   "instead",
                   arghelper_managed_interfaces);
    cmd->AddOption("show", 's',
                   "Show the current access control lists");
    cmd->AddOption("grant", 'G', "<UID | username>", true,
                   "Grant this user access to this session");
    cmd->AddOption("revoke", 'R', "<UID | username>", true,
                   "Revoke this user access from this session");
    cmd->AddOption("public-access", "<true|false>", true,
                   "Set/unset the public access flag",
                   arghelper_boolean);
    cmd->AddOption("allow-log-access", "<true|false>", true,
                   "Can users granted access also access the session log?",
                   arghelper_boolean);

    return cmd;
}



//////////////////////////////////////////////////////////////////////////
