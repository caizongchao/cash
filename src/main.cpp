/******************************************************************************
 *                       ____    _    _____                                   *
 *                      / ___|  / \  |  ___|    C++                           *
 *                     | |     / _ \ | |_       Actor                         *
 *                     | |___ / ___ \|  _|      Framework                     *
 *                      \____/_/   \_|_|                                      *
 *                                                                            *
 * Copyright (C) 2011 - 2014                                                  *
 * Dominik Charousset <dominik.charousset (at) haw-hamburg.de>                *
 *                                                                            *
 * Distributed under the terms and conditions of the BSD 3-Clause License or  *
 * (at your option) under the terms and conditions of the Boost Software      *
 * License 1.0. See accompanying files LICENSE and LICENCE_ALTERNATIVE.       *
 *                                                                            *
 * If you did not receive a copy of the license files, see                    *
 * http://opensource.org/licenses/BSD-3-Clause and                            *
 * http://www.boost.org/LICENSE_1_0.txt.                                      *
 ******************************************************************************/

#include <set>
#include <chrono>
#include <thread>
#include <iomanip>
#include <iostream>

#include "caf/all.hpp"
#include "caf/io/all.hpp"
#include "caf/shell/args.hpp"
#include "caf/probe_event/all.hpp"
#include "caf/shell/test_nodes.hpp"
#include "caf/shell/shell_actor.hpp"

#include "sash/sash.hpp"
#include "sash/libedit_backend.hpp" // our backend
#include "sash/variables_engine.hpp"

using namespace std;
using namespace caf;
using namespace probe_event;
using namespace caf::shell;

using char_iter = string::const_iterator;

inline bool empty(string& err, char_iter first, char_iter last) {
  if (first != last) {
    err = "to many arguments (none expected).";
    return false;;
  }
  return true;
}

/**
 * @param percent of progress.
 * @param filling sign. default is #
 * @param amout of signs. default is 50
 **/
string progressbar(int percent, char sign = '#', int amount = 50) {
  if (percent > 100 || percent < 0) {
    return "[ERROR]: Invalid percent in progressbar";
  }
  stringstream s;
  s << "["
    << left << setw(amount)
    << string(percent, sign)
    << "] " << right << flush;
  return s.str();
}


int main(int argc, char** argv) {
  announce_types(); // probe_event types
  announce<string>();
  announce<vector<node_data>>();
  args::net_config config;
  args::from_args(config, argc, argv);
  if(!config.valid()) {
    args::print_help();
    return 42;
  }
  { // scope of self
    scoped_actor              self;
    auto                      shellactor   = spawn<shell_actor>();
    auto nex = io::typed_remote_actor<probe_event::nexus_type>(config.host,
                                                               config.port);
    anon_send(nex, probe_event::add_listener{shellactor});
    using sash::command_result;
    using cli_type = sash::sash<sash::libedit_backend>::type;
    cli_type                  cli;
    string                    line;
    auto                      global_mode  = cli.mode_add("global", " $ ");
    auto                      node_mode    = cli.mode_add("node"  , " $ ");
    bool                      done         = false;
    vector<node_id>           visited_nodes;
    cli.mode_push("global");
    auto engine = sash::variables_engine<>::create();
    cli.add_preprocessor(engine->as_functor());
    auto get_node_data = [&](string& err) -> optional<node_data> {
      node_data nd;
      bool valid = true;
      self->sync_send(shellactor, atom("NodeData"), visited_nodes.back()).await(
        [&](node_info ni_, work_load wl_, ram_usage ru_) {
          nd.node_info = ni_;
          nd.ram_usage = ru_;
          nd.work_load = wl_;
        },
        [&](string& msg) {
          err = std::move(msg);
          valid = false;
        }
      );
      if (valid) {
        return nd;
      }
      return none;
    };
    vector<cli_type::mode_type::cmd_clause> global_cmds {
        {
          "quit", "terminates the whole thing.",
          [&](string& err, char_iter first, char_iter last) -> command_result {
            if (first != last) {
              err = "quit: to many arguments (none expected).";
              return sash::no_command;
            }
            anon_send_exit(shellactor, exit_reason::user_shutdown);
            done = true;
            return sash::executed;
          }
        },
        {
          "echo", "prints its arguments.",
          [](string&, char_iter first, char_iter last) -> command_result {
            copy(first, last, ostream_iterator<char>(cout));
            cout << endl;
            return sash::executed;
          }
        },
        {
          "clear", "clears screen.",
          [](string& err, char_iter, char_iter) {
            err = "Implementation so far to clear screen: 'ctrl + l'.";
            return sash::no_command;
          }
        },
        {
          "help", "prints this text",
          [&](string& err, char_iter first, char_iter last) -> command_result {
            if (!empty(err, first, last)) {
              return sash::no_command;
            }
            string cmd = "echo ";
            cmd += cli.current_mode().help();
            return cli.process(cmd);
          }
        },
        {
          "test-nodes", "loads static dummy-nodes.",
          [&](string& err, char_iter first, char_iter last) -> command_result {
            if (empty(err, first, last)) {
              return sash::no_command;
            }
            auto nodes = test_nodes();
            for (auto kvp : nodes) {
              auto nd = kvp.second;
              anon_send(shellactor, nd.node_info);
              anon_send(shellactor, nd.work_load);
              anon_send(shellactor, nd.ram_usage);
            }
            return sash::executed;
          }
        },
        {
          "list-nodes", "prints all available nodes.",
          [&](string& err, char_iter first, char_iter last) -> command_result {
            if (!empty(err, first, last)) {
              return sash::no_command;
            }
            self->sync_send(shellactor, atom("GetNodes")).await(
              [](std::vector<node_data>& nodes) {
                if(nodes.empty()) {
                  cout << " no nodes avaliable." << endl;
                }
                for (auto& nd : nodes) {
                  cout << to_string(nd.node_info.source_node) << endl;
                }
              }
            );
            return sash::executed;
          }
        },
        {
          "change-node", "similar to directorys you can switch between nodes.",
          [&](string& err, char_iter first, char_iter last) -> command_result {
            if (empty(err, first, last)) {
              return sash::no_command;
            }
            auto input_node = from_string<node_id>(string(first, last));
            if (!input_node) {
              err = "change-node: invalid node-id. ";
              return sash::no_command;
            }
            // TODO: check if input_node is known to shell_actor
            // TODO: check if input_node is current node
            cli.mode_push("node");
            visited_nodes.push_back(*input_node);
            return sash::executed;
          }
        },
        {
          "whereami", "prints current node you are located at.",
          [&](string& err, char_iter first, char_iter last) -> command_result {
            if (!empty(err, first, last)) {
              return sash::no_command;
            }
            if (visited_nodes.empty()) {
              err = "You are currently in globalmode."
                    " Please select a node with 'change-node <node_id>'.";
              return sash::no_command;
            }
            cout << to_string(visited_nodes.back()) << endl;
            engine->set("NODE", to_string(visited_nodes.back()));
            return sash::executed;
          }
        },
        {
          "sleep", "delay for n milliseconds",
          [&](string& err, char_iter first, char_iter last) -> command_result {
            if(empty(err, first, last)) {
              return sash::no_command;
            }
            int time = stoi(string(first,last));
            this_thread::sleep_for(chrono::milliseconds(time));
            return sash::executed;
          }
        },
        {
          "mailbox", "prints the current context of the shell's mailbox",
          [&](string& err, char_iter first, char_iter last) -> command_result {
            // TODO: implement me
          }
        },
        {
          "dequeue", "removes and prints an element from the mailbox",
          [&](string& err, char_iter first, char_iter last) -> command_result {
            // TODO: implement me
          }
        },
        {
          "pop-front", "removes and prints the oldest element from the mailbox",
          [&](string& err, char_iter first, char_iter last) -> command_result {
            // TODO: implement me
          }
        }
      };
    vector<cli_type::mode_type::cmd_clause> node_cmds {
        {
          "leave-node", "returns to global mode",
          [&](string& err, char_iter first, char_iter last) -> command_result {
            if (!empty(err, first, last)) {
              return sash::no_command;
            }
            self->sync_send(shellactor, atom("LeaveNode"));
            cli.mode_pop();
            cout << "Leaving node-mode..."
                 << endl;
            engine->unset("NODE");
            return sash::executed;
          }
        },
        // FIXME: whereiwas doesn't work!
/*        {
          "whereiwas", "prints all node-ids visited starting with least.",
          [&](string&, char_iter, char_iter) -> command_result {
            list<node_id> visited_nodes;
            self->sync_send(shellactor, atom("visited")).await(
                  on(atom("done"), arg_match) >> [&] (const list<node_id>& ids){
                    visited_nodes = ids;
                  }
            );
            int i = visited_nodes.size();
            for (const auto& node : visited_nodes) {
              cout << i << ": " << to_string(node)
                   << endl;
              i--;
            }
            return sash::executed;
          }
        }, */
        {
          "back", "changes location to previous node.",
          [&](string& err, char_iter first, char_iter last) -> command_result {
            if (!empty(err, first, last)) {
              return sash::no_command;
            }
            self->sync_send(shellactor, atom("Back")).await(
              on(atom("leave")) >> [&]() {
                cli.mode_pop();
                engine->unset("NODE");
              },
              on(atom("done"), arg_match) >> [&](const node_id& new_id) {
                engine->set("NODE", to_string(new_id));
              }
            );
            return sash::executed;
          }
        },
        {
          "work-load", "prints two bars for CPU and RAM.",
          [&](string& err, char_iter first, char_iter last) -> command_result {
            if (!empty(err, first, last)) {
              return sash::no_command;
            }
            auto nd = get_node_data(err);
            if(nd) {
              cout << "CPU: "
                   << progressbar(nd->work_load.cpu_load / 2, '#')
                   << static_cast<int>(nd->work_load.cpu_load) << "%"
                   << endl;
              // ram_usage
              auto used_ram_in_percent =
              static_cast<size_t>((nd->ram_usage.in_use * 100.0)
                                   / nd->ram_usage.available);
              cout << "RAM: "
                   << progressbar(used_ram_in_percent / 2, '#')
                   << nd->ram_usage.in_use
                   << "/"
                   << nd->ram_usage.available
                   << endl;
              return sash::executed;
            }
            return sash::no_command;
          }
        },
        {
          "statistics", "prints statistics of current node.",
          [&](string& err, char_iter first, char_iter last) -> command_result {
            if(!empty(err, first, last)) {
              return sash::no_command;
            }
            auto nd = get_node_data(err);
            if (nd) {
              // node_info
              cout << setw(21) << "Node-ID:  "
                   << setw(50) << left
                   << to_string((nd->node_info.source_node)) << right
                   << endl
                   << setw(21) << "Hostname:  "
                   << nd->node_info.hostname
                   << endl
                   << setw(21) << "Operatingsystem:  "
                   << nd->node_info.os
                   << endl
                   << setw(20) << "CPU statistics: "
                   << setw(3)  << "#"
                   << setw(10) << "Core No"
                   << setw(12) << "MHz/Core"
                   << endl;
                int i = 1;
                for (const auto& cpu : nd->node_info.cpu) {
                  cout << setw(23) << i
                       << setw(10) << cpu.num_cores
                       << setw(12) << cpu.mhz_per_core
                       << endl;
                  i++;
                }
              // work_load
              cout << setw(20) << "Processes: "
                   << setw(3)  << nd->work_load.num_processes
                   << endl
                   << setw(20) << "Actors: "
                   << setw(3)  << nd->work_load.num_actors
                   << endl
                   << setw(20) << "CPU: "
                   << setw(2)
                   << progressbar(nd->work_load.cpu_load/2, '#')
                   << " "
                   << static_cast<int>(nd->work_load.cpu_load) << "%"
                   << endl;
              // ram_usage
              auto used_ram_in_percent =
                static_cast<size_t>((nd->ram_usage.in_use * 100.0)
                                    / nd->ram_usage.available);
              cout << setw(20) << "RAM: "
                   << setw(2)
                   << progressbar(used_ram_in_percent / 2, '#')
                   << nd->ram_usage.in_use
                   << "/"
                   << nd->ram_usage.available
                   << endl;
              return sash::executed;
            }
            return sash::no_command;
          }
        },
        { // TODO: adjust ipv6 output
          "interfaces", "show interface information.",
          [&](string& err, char_iter first, char_iter last) -> command_result {
            if(!empty(err, first, last)) {
              return sash::no_command;
            }
            auto nd = get_node_data(err);
            if (!nd) {
              err = "interfaces: unexpected error.";
              return sash::no_command;
            } else {
              int intend = 8;
              for (auto interface : nd->node_info.interfaces) {
                cout << setw(intend) << "Name: "
                     << interface.name
                     << endl
                     << setw(intend) << "MAC: "
                     << interface.hw_addr
                     << endl
                     << setw(intend) << "IPv4: "
                     << interface.ipv4_addr
                     << endl
                     << setw(intend) << "IPv6: ";
                     for (auto ipv6 : interface.ipv6_addrs) {
                       cout << ipv6
                            << endl;
                     }
                     cout << endl;
              }
            }
            return sash::executed;
          }
        },
        {
          "send", "sends a message to an actor",
          [&](string& err, char_iter first, char_iter last) -> command_result {
            // TODO: implement me
          }
        }
    };
    global_mode->add_all(global_cmds);
    node_mode->add_all(global_cmds);
    node_mode->add_all(node_cmds);
    while (!done) {
      cli.read_line(line);
      switch (cli.process(line)) {
        default:
          break;
        case sash::nop:
          break;
        case sash::executed:
          cli.append_to_history(line);
          break;
        case sash::no_command:
          cli.append_to_history(line);
          cout << cli.last_error()
               << endl;
          break;
      }
    }
  } // scope of self
  await_all_actors_done();
  shutdown();
}
