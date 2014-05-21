/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "RouteController.h"

#include "Fwmark.h"
#include "NetdConstants.h"

#include <linux/rtnetlink.h>
#include <logwrap/logwrap.h>
#include <net/if.h>

namespace {

const uint32_t RULE_PRIORITY_PER_NETWORK_EXPLICIT  = 13000;
const uint32_t RULE_PRIORITY_PER_NETWORK_INTERFACE = 14000;
const uint32_t RULE_PRIORITY_PER_NETWORK_NORMAL    = 17000;
const uint32_t RULE_PRIORITY_DEFAULT_NETWORK       = 19000;
const uint32_t RULE_PRIORITY_MAIN                  = 20000;
const uint32_t RULE_PRIORITY_UNREACHABLE           = 21000;

uint32_t getRouteTableForInterface(const char* interface) {
    uint32_t index = if_nametoindex(interface);
    return index ? index + RouteController::ROUTE_TABLE_OFFSET_FROM_INDEX : 0;
}

// Adds or removes a routing rule for IPv4 and IPv6.
//
// + If |table| is non-zero, the rule points at the specified routing table. Otherwise, the rule
//   returns ENETUNREACH.
// + If |mask| is non-zero, the rule matches the specified fwmark and mask. Otherwise, |fwmark| is
//   ignored.
// + If |interface| is non-NULL, the rule matches the specified outgoing interface.
bool runIpRuleCommand(const char* action, uint32_t priority, uint32_t table, uint32_t fwmark,
                      uint32_t mask, const char* interface) {
    char priorityString[UINT32_STRLEN];
    snprintf(priorityString, sizeof(priorityString), "%u", priority);

    char tableString[UINT32_STRLEN];
    snprintf(tableString, sizeof(tableString), "%u", table);

    char fwmarkString[sizeof("0x12345678/0x12345678")];
    snprintf(fwmarkString, sizeof(fwmarkString), "0x%x/0x%x", fwmark, mask);

    const char* version[] = {"-4", "-6"};
    for (size_t i = 0; i < ARRAY_SIZE(version); ++i) {
        int argc = 0;
        const char* argv[16];

        argv[argc++] = IP_PATH;
        argv[argc++] = version[i];
        argv[argc++] = "rule";
        argv[argc++] = action;
        argv[argc++] = "priority";
        argv[argc++] = priorityString;
        if (table) {
            argv[argc++] = "table";
            argv[argc++] = tableString;
        } else {
            argv[argc++] = "unreachable";
        }
        if (mask) {
            argv[argc++] = "fwmark";
            argv[argc++] = fwmarkString;
        }
        if (interface) {
            argv[argc++] = "oif";
            argv[argc++] = interface;
        }
        if (android_fork_execvp(argc, const_cast<char**>(argv), NULL, false, false)) {
            return false;
        }
    }

    return true;
}

bool runIpRouteCommand(const char* action, uint32_t table, const char* interface,
                       const char* destination, const char* nexthop) {
    char tableString[UINT32_STRLEN];
    snprintf(tableString, sizeof(tableString), "%u", table);

    int argc = 0;
    const char* argv[16];

    argv[argc++] = IP_PATH;
    argv[argc++] = "route";
    argv[argc++] = action;
    argv[argc++] = "table";
    argv[argc++] = tableString;
    if (destination) {
        argv[argc++] = destination;
        argv[argc++] = "dev";
        argv[argc++] = interface;
        if (nexthop) {
            argv[argc++] = "via";
            argv[argc++] = nexthop;
        }
    }

    return !android_fork_execvp(argc, const_cast<char**>(argv), NULL, false, false);
}

bool modifyPerNetworkRules(unsigned netId, const char* interface, Permission permission, bool add,
                           bool modifyIptables) {
    uint32_t table = getRouteTableForInterface(interface);
    if (!table) {
        return false;
    }

    const char* action = add ? ADD : DEL;

    Fwmark fwmark;
    fwmark.permission = permission;

    Fwmark mask;
    mask.permission = permission;

    // A rule to route traffic based on a chosen outgoing interface.
    //
    // Supports apps that use SO_BINDTODEVICE or IP_PKTINFO options and the kernel that already
    // knows the outgoing interface (typically for link-local communications).
    if (!runIpRuleCommand(action, RULE_PRIORITY_PER_NETWORK_INTERFACE, table, fwmark.intValue,
                          mask.intValue, interface)) {
        return false;
    }

    // A rule to route traffic based on the chosen network.
    //
    // This is for sockets that have not explicitly requested a particular network, but have been
    // bound to one when they called connect(). This ensures that sockets connected on a particular
    // network stay on that network even if the default network changes.
    fwmark.netId = netId;
    mask.netId = FWMARK_NET_ID_MASK;
    if (!runIpRuleCommand(action, RULE_PRIORITY_PER_NETWORK_NORMAL, table, fwmark.intValue,
                          mask.intValue, NULL)) {
        return false;
    }

    // A rule to route traffic based on an explicitly chosen network.
    //
    // Supports apps that use the multinetwork APIs to restrict their traffic to a network.
    //
    // We don't really need to check the permission bits of the fwmark here, as they would've been
    // checked at the time the netId was set into the fwmark, but we do so to be consistent.
    fwmark.explicitlySelected = true;
    mask.explicitlySelected = true;
    if (!runIpRuleCommand(action, RULE_PRIORITY_PER_NETWORK_EXPLICIT, table, fwmark.intValue,
                          mask.intValue, NULL)) {
        return false;
    }

    // An iptables rule to mark incoming packets on a network with the netId of the network.
    //
    // This is so that the kernel can:
    // + Use the right fwmark for (and thus correctly route) replies (e.g.: TCP RST, ICMP errors,
    //   ping replies).
    // + Mark sockets that accept connections from this interface so that the connection stays on
    //   the same interface.
    if (modifyIptables) {
        action = add ? "-A" : "-D";
        char markString[UINT32_HEX_STRLEN];
        snprintf(markString, sizeof(markString), "0x%x", netId);
        if (execIptables(V4V6, "-t", "mangle", action, "INPUT", "-i", interface, "-j", "MARK",
                         "--set-mark", markString, NULL)) {
            return false;
        }
    }

    return true;
}

bool modifyDefaultNetworkRules(const char* interface, Permission permission, const char* action) {
    uint32_t table = getRouteTableForInterface(interface);
    if (!table) {
        return false;
    }

    Fwmark fwmark;
    fwmark.netId = 0;
    fwmark.permission = permission;

    Fwmark mask;
    mask.netId = FWMARK_NET_ID_MASK;
    mask.permission = permission;

    return runIpRuleCommand(action, RULE_PRIORITY_DEFAULT_NETWORK, table, fwmark.intValue,
                            mask.intValue, NULL);
}

bool modifyRoute(const char* interface, const char* destination, const char* nexthop,
                 const char* action) {
    uint32_t table = getRouteTableForInterface(interface);
    if (!table) {
        return false;
    }

    if (!runIpRouteCommand(action, table, interface, destination, nexthop)) {
        return false;
    }

    // If there's no nexthop, this is a directly connected route. Add it to the main table also, to
    // let the kernel find it when validating nexthops when global routes are added. Don't do this
    // for IPv6, since all directly-connected routes in v6 are link-local and should already be in
    // the main table.
    // TODO: A failure here typically means that the route already exists in the main table, so we
    // ignore it. It's wrong to ignore other kinds of failures, but we have no way to distinguish
    // them based on the return status of the 'ip' command. Fix this situation by ignoring errors
    // only when action == ADD && error == EEXIST.
    if (!nexthop && !strchr(destination, ':')) {
        runIpRouteCommand(action, RT_TABLE_MAIN, interface, destination, NULL);
    }

    return true;
}

bool flushRoutes(const char* interface) {
    uint32_t table = getRouteTableForInterface(interface);
    if (!table) {
        return false;
    }

    return runIpRouteCommand("flush", table, NULL, NULL, NULL);
}

}  // namespace

void RouteController::Init() {
    // Add a new rule to look up the 'main' table, with the same selectors as the "default network"
    // rule, but with a lower priority. Since the default network rule points to a table with a
    // default route, the rule we're adding will never be used for normal routing lookups. However,
    // the kernel may fall-through to it to find directly-connected routes when it validates that a
    // nexthop (in a route being added) is reachable.
    Fwmark fwmark;
    fwmark.netId = 0;

    Fwmark mask;
    mask.netId = FWMARK_NET_ID_MASK;

    runIpRuleCommand(ADD, RULE_PRIORITY_MAIN, RT_TABLE_MAIN, fwmark.intValue, mask.intValue, NULL);

// TODO: Uncomment once we are sure everything works.
#if 0
    // Add a rule to preempt the pre-defined "from all lookup main" rule. This ensures that packets
    // that are already marked with a specific NetId don't fall-through to the main table.
    runIpRuleCommand(ADD, RULE_PRIORITY_UNREACHABLE, 0, 0, 0, NULL);
#endif
}

bool RouteController::addInterfaceToNetwork(unsigned netId, const char* interface,
                                            Permission permission) {
    return modifyPerNetworkRules(netId, interface, permission, true, true);
}

bool RouteController::removeInterfaceFromNetwork(unsigned netId, const char* interface,
                                                 Permission permission) {
    return modifyPerNetworkRules(netId, interface, permission, false, true) &&
           flushRoutes(interface);
}

bool RouteController::modifyNetworkPermission(unsigned netId, const char* interface,
                                              Permission oldPermission, Permission newPermission) {
    // Add the new rules before deleting the old ones, to avoid race conditions.
    return modifyPerNetworkRules(netId, interface, newPermission, true, false) &&
           modifyPerNetworkRules(netId, interface, oldPermission, false, false);
}

bool RouteController::addToDefaultNetwork(const char* interface, Permission permission) {
    return modifyDefaultNetworkRules(interface, permission, ADD);
}

bool RouteController::removeFromDefaultNetwork(const char* interface, Permission permission) {
    return modifyDefaultNetworkRules(interface, permission, DEL);
}

bool RouteController::addRoute(const char* interface, const char* destination,
                               const char* nexthop) {
    return modifyRoute(interface, destination, nexthop, ADD);
}

bool RouteController::removeRoute(const char* interface, const char* destination,
                                  const char* nexthop) {
    return modifyRoute(interface, destination, nexthop, DEL);
}
