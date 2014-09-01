#include "caf/shell/shell_actor.hpp"


namespace caf {

using namespace std;
using namespace probe_event;

namespace shell {

shell_actor::shell_actor() {
  // nop
}

bool shell_actor::is_known(const node_id& id) {
  return m_known_nodes.count(id) != 0;
}

/**
 * @brief shell_actor::add
 * @param ni - node_id
 * @return true when node_id wasn't known and has been added.
 */
bool shell_actor::add(const node_info& ni) {
  if (is_known(ni.source_node)) {
    return false;
  }
  node_data nd;
  nd.node_info = ni;
  m_known_nodes.insert(std::make_pair(ni.source_node, nd));
  return true;
}

/**
 * @brief shell_actor::add
 * @param wl - work_load
 * @return true when id was known and added or false when work_load drops.
 */
bool shell_actor::set(const work_load& wl) {
  auto kvp = m_known_nodes.find(wl.source_node);
  if (kvp == m_known_nodes.end()) {
    return false;
  }
  kvp->second.work_load = wl;
  return true;
}

/**
 * @brief shell_actor::add
 * @param ru - ram_usage
 * @return true when node_id wasn't known and has been added.
 */
bool shell_actor::set(const ram_usage& ru) {
  auto kvp = m_known_nodes.find(ru.source_node);
  if (kvp == m_known_nodes.end()) {
    return false;
  }
  kvp->second.ram_usage = ru;
  return true;
}

behavior shell_actor::make_behavior() {
  return {
    // nexus communication
    [=](const probe_event::new_message& ) {
      //aout(this) << "new message" << endl;
    },
    [=](const probe_event::new_route& nr) {
      //aout(this) << "new message" << endl;
    },
    [=](const probe_event::node_info& ni) {
      aout(this) << "new node_info" << endl;
      if (!add(ni)) {
        aout(this) << "droped node_info: " << to_string(ni.source_node)
                   << endl;
      }
    },
    [=](const probe_event::work_load& wl) {
      if (set(wl)) {
        aout(this) << "droped work_load: " << to_string(wl.source_node)
                   << endl;
      }
    },
    [=](const probe_event::ram_usage& ru) {
      if (set(ru)) {
        aout(this) << "droped ram_usage: " << to_string(ru.source_node)
                   << endl;
      }
    },
    // shell communication
    on(atom("AddTest"), arg_match) >> [&](node_id id, node_data data) {
      add(data.node_info);
      set(data.work_load);
      set(data.ram_usage);
    },
    //[](const get_all_nodes&) -> std::vector<node_data> {
    on(atom("GetNodes")) >> [&]() -> std::vector<node_data> {
      std::vector<node_data> result;
      for (const auto& kvp : m_known_nodes) {
        result.push_back(kvp.second);
      }
      return result;
    },
    // TODO: refactor to main
    on(atom("ChangeNode"), arg_match) >> [&](node_id input_node) {
      if (m_known_nodes.empty()) {
        return make_message(atom("cnfail"), string("No nodes known."));
      } else if (m_known_nodes.count(input_node) == 0) {
        return make_message(atom("cnfail"), string("Given node is unkown."));
      } else {
        if (m_visited_nodes.back() != input_node) {
          m_visited_nodes.push_back(input_node);
        }
        return make_message(atom("cncorrect"), input_node);
      }
    },
    on(atom("WhereAmI")) >> [=] {
      return m_visited_nodes.empty()
          ? make_message(atom("waifail"), "You are currently in globalmode. "
                                          "You can select a node "
                                          "with 'change-node <node_id>'.")
          : make_message(atom("waicorrect"),
                         to_string(m_visited_nodes.back()));
    },
    on(atom("NodeData")) >> [&]() -> message { // optional<node_data>
      auto search = m_known_nodes.find(m_visited_nodes.back());
      if (search != m_known_nodes.end()) {
        auto nd = search->second;
        auto ni = nd.node_info;
        auto wl = nd.work_load;
        auto ru = nd.ram_usage;
        return make_message(ni, wl, ru);
      } else {
        return make_message("Node not found");
      }
    },
    on(atom("LeaveNode")) >> [&] {
      m_visited_nodes = list<node_id>();
      return make_message(atom("done"));
    },
    on(atom("Back")) >> [&]() {
      if (m_visited_nodes.size() <= 1) {
        m_visited_nodes = list<node_id>();
        return make_message(atom("leave"));
      } else {
        m_visited_nodes.pop_back();
        return make_message(atom("done"), m_visited_nodes.back());
      }
    },
    others() >> [=] {
      aout(this) << "Received from sender: "
                 << to_string(last_sender())
                 << endl << "an unexpected message. "
                 << to_string(last_dequeued())
                 << endl << endl;
    }
  };
}

} // namespace shell
} // namespace caf
