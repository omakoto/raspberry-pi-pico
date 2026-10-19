// Access to the generated SwitchBot command tables (see switchbot_commands.cpp, produced by
// tools/gen_switchbot_commands.py). Internal to the dispatcher core: users call commands_for() instead.

#pragma once

#include <cstddef>

namespace dispatcher {
namespace generated {

struct CommandEntry {
  const char* command;
  const char* parameter;
  const char* description;
};

// The commands of one device type or IR appliance type; `rows` is null when `count` is zero.
struct CommandTable {
  const char* type;
  const CommandEntry* rows;
  size_t count;
};

// Sorted by type name (byte order), so lookups may bisect or scan.
extern const CommandTable kDeviceCommandTables[];
extern const size_t kDeviceCommandTableCount;
extern const CommandTable kIrCommandTables[];
extern const size_t kIrCommandTableCount;
// Commands every IR appliance supports.
extern const CommandEntry kIrCommonCommands[];
extern const size_t kIrCommonCommandCount;

}  // namespace generated
}  // namespace dispatcher
