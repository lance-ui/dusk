#pragma once

#include <string>
#include <vector>

namespace dusk {

class Server;

struct AutostartApp {
	std::string file;   // e.g. "discord.desktop"
	std::string exec;
	bool run;           // enabled and meant for Dusk
};

void start_keyring(Server &server);
void start_polkit_agent(Server &server);
std::vector<AutostartApp> list_autostart();
void run_autostart(Server &server);

} // namespace dusk
