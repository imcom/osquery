// Copyright 2004-present Facebook. All Rights Reserved.

#include <fstream>

#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/trim.hpp>

#include "osquery/database.h"
#include "osquery/filesystem.h"

namespace osquery {
namespace tables {

const std::string kLinuxArpTable = "/proc/net/arp";

QueryData genArpCache() {
  QueryData results;

  boost::filesystem::path arp_path = kLinuxArpTable;
  if (!osquery::isReadable(arp_path).ok()) {
    VLOG(1) << "Cannot read arp table.";
    return results;
  }

  std::ifstream fd(arp_path.string(), std::ios::in | std::ios::binary);
  std::string line;

  if (fd.fail() || fd.eof()) {
    VLOG(1) << "Empty or failed arp table.";
    return results;
  }

  // Read the header line.
  std::getline(fd, line, '\n');
  while (!(fd.fail() || fd.eof())) {
    std::getline(fd, line, '\n');

    // IP address, HW type, Flags, HW address, Mask Device
    std::vector<std::string> fields;
    boost::split(fields, line, boost::is_any_of(" "), boost::token_compress_on);
    for (auto& f : fields) {
      // Inline trim each split.
      boost::trim(f);
    }

    if (fields.size() != 6) {
      // An unhandled error case.
      continue;
    }

    Row r;
    r["address"] = fields[0];
    r["mac"] = fields[3];
    r["interface"] = fields[5];

    // Note: it's also possible to detect publish entries (ATF_PUB).
    if (fields[2] == "0x6") {
      // The string representation of ATF_COM | ATF_PERM.
      r["permanent"] = "1";
    } else {
      r["permanent"] = "0";
    }

    results.push_back(r);
  }

  return results;
}
}
}
