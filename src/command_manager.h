#ifndef TARGET_REDUCTION_COMMAND_MANAGER_H
#define TARGET_REDUCTION_COMMAND_MANAGER_H

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <cli/cli.h>
#include "aig_manager.h"

using CommandHandler = std::function<int(AIGMan &, const std::vector<std::string> &)>;

class CmdMan {
public:
    CmdMan() { registerAllCommands(); }

    void registerAllCommands();
    void registerCommand(const std::string & cmd, const CommandHandler & cmdHandlerFunc);
    void launchCommand(AIGMan & aigman, const std::string & cmd, const std::vector<std::string> & vLiterals);
    void cliMenuAddCommands(AIGMan & aigman, std::unique_ptr<cli::Menu> & cliMenu);

private:
    std::unordered_map<std::string, CommandHandler> handlers;
    std::vector<std::string> commandOrder;
};

#endif
