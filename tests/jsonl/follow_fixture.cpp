// Real mapped producer controlled by its receiving subprocess; no timing guesses.
#include <sub0log/log.hpp>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>

int main(int argc, char** argv)
{
    if (argc != 3) { return 2; }
    auto logger = sub0log::Logger::create({.directory_ = argv[1]});
    if (!logger.valid()) { return 1; }
    const auto emit = [&](const std::uint64_t value) {
        const sub0log::Logger::ScopedBind bind{logger};
        sub0log_info(sub0log::SubsystemId{7}, "event {}", value);
    };
    if (std::string{argv[2]} == "static") {
        emit(0);
        emit(1);
        std::thread worker{[&] { emit(2); }};
        worker.join();
    } else {
        std::cout << "ready" << std::endl;
        for (std::string command; std::getline(std::cin, command);) {
            if (command == "quit") { break; }
            emit(std::stoull(command));
            std::cout << "committed" << std::endl;
        }
    }
    return logger.stats().droppedRecords_ == 0 ? 0 : 1;
}
