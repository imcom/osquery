// Copyright 2004-present Facebook. All Rights Reserved.

#include <string>
#include <iomanip>

#include <ifaddrs.h>
#include <stdio.h>
#include <stdlib.h>

#include <arpa/inet.h>
#include <net/if_dl.h>
#include <net/route.h>
#include <sys/sysctl.h>

#include <boost/algorithm/string/trim.hpp>
#include <boost/lexical_cast.hpp>

#include <glog/logging.h>

#include "osquery/core.h"
#include "osquery/database.h"
#include "osquery/tables/networking/utils.h"

namespace osquery {
namespace tables {

typedef std::pair<int, std::string> RouteType;
typedef std::map<int, std::string> InterfaceMap;
typedef std::vector<struct sockaddr *> AddressMap;

const std::string kDefaultRoute = "0.0.0.0";

const std::vector<RouteType> kRouteTypes = {
    std::make_pair(RTF_LOCAL, "local"),
    std::make_pair(RTF_GATEWAY, "gateway"),
    std::make_pair(RTF_DYNAMIC, "dynamic"),
    std::make_pair(RTF_MODIFIED, "modified"),
    std::make_pair(RTF_STATIC, "static"),
    std::make_pair(RTF_BLACKHOLE, "blackhole"),
    std::make_pair(RTF_ROUTER, "router"),
    std::make_pair(RTF_PROXY, "proxy"),
};

const std::vector<RouteType> kArpTypes = {
    std::make_pair(RTF_LLINFO, "linklayer"), };

InterfaceMap genInterfaceMap() {
  InterfaceMap ifmap;

  struct ifaddrs *if_addrs, *if_addr;
  struct sockaddr_dl *sdl;

  if (getifaddrs(&if_addrs) != 0) {
    LOG(ERROR) << "Failed to create interface map, getifaddrs() failed.";
    return ifmap;
  }

  InterfaceMap::iterator it = ifmap.begin();
  for (if_addr = if_addrs; if_addr != nullptr; if_addr = if_addr->ifa_next) {
    if (if_addr->ifa_addr != nullptr &&
        if_addr->ifa_addr->sa_family == AF_LINK) {
      std::string route_type = std::string(if_addr->ifa_name);
      sdl = (struct sockaddr_dl *)if_addr->ifa_addr;
      ifmap.insert(it, std::make_pair(sdl->sdl_index, route_type));
    }
  }

  freeifaddrs(if_addrs);
  return ifmap;
}

Status genRoute(const struct rt_msghdr *route,
                const AddressMap &addr_map,
                Row &r) {
  r["flags"] = INTEGER(route->rtm_flags);
  r["mtu"] = INTEGER(route->rtm_rmx.rmx_mtu);

  if ((route->rtm_addrs & RTA_DST) == RTA_DST) {
    r["destination"] = ipAsString(addr_map[RTAX_DST]);
  }

  if ((route->rtm_addrs & RTA_GATEWAY) == RTA_GATEWAY) {
    r["gateway"] = ipAsString(addr_map[RTAX_GATEWAY]);
  }

  if (r["destination"] == kDefaultRoute) {
    r["netmask"] = "0";
  } else if ((route->rtm_addrs & RTA_NETMASK) == RTA_NETMASK) {
    addr_map[RTAX_NETMASK]->sa_family = addr_map[RTAX_DST]->sa_family;
    r["netmask"] = INTEGER(netmaskFromIP(addr_map[RTAX_NETMASK]));
  } else {
    if (addr_map[RTAX_DST]->sa_family == AF_INET6) {
      r["netmask"] = "128";
    } else {
      r["netmask"] = "32";
    }
  }

  // Fields not supported by OSX routes:
  r["source"] = "";
  r["metric"] = "0";
  return Status(0, "OK");
}

Status genArp(const struct rt_msghdr *route,
              const AddressMap &addr_map,
              Row &r) {
  if (addr_map[RTAX_DST]->sa_family != AF_INET) {
    return Status(1, "Not in ARP cache");
  }

  // The cache will always know the address.
  r["address"] = ipAsString(addr_map[RTAX_DST]);

  auto sdl = (struct sockaddr_dl *)addr_map[RTA_DST];
  if (sdl->sdl_alen > 0) {
    r["mac"] = macAsString(LLADDR(sdl));
  } else {
    r["mac"] = "incomplete";
  }

  // Note: also possible to detect published.
  if (route->rtm_rmx.rmx_expire == 0) {
    r["permanent"] = "1";
  } else {
    r["permanent"] = "0";
  }

  return Status(0, "OK");
}

void genRouteTableType(RouteType type, InterfaceMap ifmap, QueryData &results) {
  size_t table_size;
  int mib[] = {CTL_NET, PF_ROUTE, 0, AF_UNSPEC, NET_RT_FLAGS, type.first};
  if (sysctl(mib, sizeof(mib) / sizeof(int), nullptr, &table_size, nullptr, 0) <
      0) {
    return;
  }

  if (table_size == 0) {
    return;
  }

  char *table, *p;
  table = (char *)malloc(table_size);
  if (sysctl(mib, sizeof(mib) / sizeof(int), table, &table_size, nullptr, 0) <
      0) {
    free(table);
    return;
  }

  struct rt_msghdr *route;
  for (p = table; p < table + table_size; p += route->rtm_msglen) {
    route = (struct rt_msghdr *)p;
    auto sa = (struct sockaddr *)(route + 1);

    // Populate route's sockaddr table (dest, gw, mask).
    AddressMap addr_map;
    for (int i = 0; i < RTAX_MAX; i++) {
      if (route->rtm_addrs & (1 << i)) {
        addr_map.push_back(sa);
        sa = (struct sockaddr *)((char *)sa + (sa->sa_len));
      } else {
        addr_map.push_back(nullptr);
      }
    }

    Row r;
    // Both route and arp tables may include an interface.
    if ((route->rtm_addrs & RTA_GATEWAY) == RTA_GATEWAY) {
      auto sdl = (struct sockaddr_dl *)addr_map[RTAX_GATEWAY];
      r["interface"] = ifmap[(int)sdl->sdl_index];
    }

    Status row_status;
    if (type.second != "linklayer") {
      // Set the type of route for the route tables only.
      r["type"] = type.second;
      row_status = genRoute(route, addr_map, r);
    } else {
      row_status = genArp(route, addr_map, r);
    }

    if (row_status.ok()) {
      results.push_back(r);
    }
  }

  free(table);
}

QueryData genArpCache() {
  QueryData results;
  InterfaceMap ifmap;

  ifmap = genInterfaceMap();
  for (const auto &arp_type : kArpTypes) {
    genRouteTableType(arp_type, ifmap, results);
  }

  return results;
}

QueryData genRoutes() {
  QueryData results;
  InterfaceMap ifmap;

  // Need a map from index->name for each route entry.
  ifmap = genInterfaceMap();
  for (const auto &route_type : kRouteTypes) {
    genRouteTableType(route_type, ifmap, results);
  }

  return results;
}
}
}
