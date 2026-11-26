#include "ChildProcess.h"
#include <cassert>
#include <fcntl.h>
#include <unistd.h>
#include <iostream>
#include <thread>
using namespace dockpipe::desktop;
int main()
{
    auto child = ChildProcess::start({"/bin/cat"}, true);
    const std::vector<std::uint8_t> frame{2, 1, 2, 3, 4};
    assert(writeRecord(child->input(), frame, 4097, std::chrono::steady_clock::now() + std::chrono::seconds(1)));
    RecordReader reader;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    RecordReader::Outcome outcome;
    do {
        outcome = reader.poll(child->output(), 4097);
        if (outcome == RecordReader::Outcome::Pending)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (outcome == RecordReader::Outcome::Pending && std::chrono::steady_clock::now() < deadline);
    assert(outcome == RecordReader::Outcome::Complete && reader.take() == frame);
    assert(child->stop());
    assert(child->stop());
    int descriptors[2];
    assert(pipe(descriptors) == 0);
    fcntl(descriptors[0], F_SETFL, O_NONBLOCK);
    const std::uint8_t invalid[]{0, 0, 0, 3, 1};
    assert(write(descriptors[1], invalid, sizeof(invalid)) == sizeof(invalid));
    close(descriptors[1]);
    RecordReader partial;
    assert(partial.poll(descriptors[0], 64) == RecordReader::Outcome::Invalid);
    close(descriptors[0]);
    assert(pipe(descriptors) == 0);
    fcntl(descriptors[0], F_SETFL, O_NONBLOCK);
    const std::uint8_t oversized[]{0, 0, 1, 0};
    assert(write(descriptors[1], oversized, sizeof(oversized)) == sizeof(oversized));
    RecordReader bounded;
    assert(bounded.poll(descriptors[0], 64) == RecordReader::Outcome::Invalid);
    close(descriptors[0]);
    close(descriptors[1]);
    std::cout << "PASS private process pipes, complete/partial/oversize records, deterministic child cleanup\n";
}
