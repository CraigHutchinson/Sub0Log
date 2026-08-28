// 10_child_capture_git.cpp -- capturing a real tool you did not write, and
// making decisions about its output as it streams past.
//
// 06_child_capture.cpp teaches the *mechanism* with a synthetic `/bin/sh -c`
// script. This teaches the *use case*: a program shells out to a real
// third-party executable it does not control, wants that tool's output in
// its own log with attribution (R5.5), and wants to react to specific
// lines while they are still arriving rather than after the fact (R5.6) --
// noticing a value worth keeping, noticing a failure worth flagging, and
// throwing away a line that is not signal. `git` is that real tool here,
// chosen because it is present on every CI runner this ladder builds on and
// because its output is genuinely worth reacting to, not merely printable.
//
// Every interceptor below makes a decision a real supervisor makes:
//   - `git commit`'s summary line names the commit it just made. Harvesting
//     the short hash out of it as it streams past is cheaper and more
//     honest than shelling out to `git rev-parse HEAD` afterwards -- the
//     hash a supervisor wants is the one the command just told it, not a
//     second round trip to ask again.
//   - `git checkout <branch that does not exist>` is made to fail on
//     purpose, and the interceptor notices the specific "pathspec did not
//     match" line rather than only the nonzero exit code -- the same way a
//     real supervisor distinguishes "this tool failed" from "this tool
//     failed *because of X*", which is usually the thing worth alerting on.
//   - `git log`'s default format includes a "Signed-off-by:" trailer this
//     example puts in its own commit messages on purpose -- boilerplate a
//     real log-parsing supervisor filters constantly, suppressed here the
//     same way 06 suppresses "NOISE-" lines, and counted rather than
//     silently gone (R9.1).
//
// Self-contained and deterministic: everything below happens inside a
// throwaway repository this program creates in a temp directory, with
// `user.email`/`user.name` set with --local (redundant with git's own
// default inside a repo, but said explicitly so this file does not quietly
// depend on it) and commit signing switched off the same way, so this
// example cannot read, depend on, or disturb anyone's real git identity or
// signing setup -- including this very checkout's. It never touches the
// Sub0Log repository it happens to be sitting in, makes no network call,
// and never runs `git clone`/`fetch`/`push`. The commit hashes it harvests
// are never hard-coded (they cannot be -- git derives them from a tree and
// a timestamp): every check below is that a harvested value is *shaped*
// like a hash and independently confirmed by `git log`, which is what
// keeps this deterministic without knowing the hashes in advance.
//
// Requirements demonstrated: R5.5 (attribution of a non-cooperating real
// child), R5.6 (interception: harvest, notice-a-failure, suppress), R9.1
// (a suppressed line is counted, never silently gone).

#include <sub0log/child.hpp>
#include <sub0log/context.hpp>
#include <sub0log/instance.hpp>
#include <sub0log/reader.hpp>
#include <sub0log/wire.hpp>

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#  include <process.h>
#  define SUB0LOG_EXAMPLE_PID() static_cast<unsigned long>(::_getpid())
#else
#  include <unistd.h>
#  define SUB0LOG_EXAMPLE_PID() static_cast<unsigned long>(::getpid())
#endif

namespace {

constexpr sub0log::SubsystemId cGit{7};

std::filesystem::path makeScratchDir(const char* const tag)
{
    auto dir = std::filesystem::temp_directory_path()
             / ("sub0log-example-" + std::string{tag} + "-" + std::to_string(SUB0LOG_EXAMPLE_PID()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

std::filesystem::path onlySegment(const std::filesystem::path& dir)
{
    for (const auto& entry : std::filesystem::directory_iterator{dir}) {
        if (entry.path().extension() == ".s0l") {
            return entry.path();
        }
    }
    return {};
}

std::vector<std::byte> slurp(const std::filesystem::path& path)
{
    std::ifstream in{path, std::ios::binary};
    std::vector<char> raw{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    const auto* first = reinterpret_cast<const std::byte*>(raw.data());
    return {first, first + raw.size()};
}

// A commit summary line looks like "[master (root-commit) 1a2b3c4] subject"
// or "[master 89abcde] subject" for every commit after the first. The short
// hash is always the last whitespace-separated token before the closing
// ']', regardless of branch name or whether "(root-commit)" is present --
// parsing it that way, rather than assuming a fixed word count, is what
// keeps this independent of git's own branch-naming defaults.
std::optional<std::string> harvestShortHash(const std::string_view line)
{
    if (line.empty() || line.front() != '[') {
        return std::nullopt;
    }
    const auto close = line.find(']');
    if (close == std::string_view::npos) {
        return std::nullopt;
    }
    const auto space = line.rfind(' ', close - 1);
    if (space == std::string_view::npos) {
        return std::nullopt;
    }
    const auto hash = line.substr(space + 1, close - space - 1);
    if (hash.empty()) {
        return std::nullopt;
    }
    for (const char c : hash) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return std::nullopt;
        }
    }
    return std::string{hash};
}

// Runs one git subcommand with its stdout/stderr captured and attributed
// (R5.5), inside the throwaway repository, printing what it teaches as it
// goes. `interceptor` gets every line before it is written (R5.6) -- pass
// nothing to log everything unfiltered.
sub0log::ChildProcess::ExitStatus runGit(const std::filesystem::path& repo, std::vector<std::string> args,
                                          const std::string_view label,
                                          sub0log::LineInterceptor interceptor = {})
{
    std::vector<std::string> argv{"git"};
    argv.insert(argv.end(), args.begin(), args.end());

    sub0log::ChildOptions options{
        .argv_ = std::move(argv),
        .workingDirectory_ = repo.string(),
        .onLine_ = std::move(interceptor),
    };
    auto child = sub0log::ChildProcess::spawn(options);
    if (!child.valid()) {
        std::fprintf(stderr, "could not spawn git %s: %s\n", label.data(), child.error().what_.data());
        return {-1, 0};
    }
    const auto status = child.wait();
    std::printf("$ git %-28s -> exit=%d\n", std::string{label}.c_str(), status.exitCode_);
    return status;
}

} // namespace

int main()
{
    // --- 0. is git even here? --------------------------------------------
    //
    // An *example* teaches a capability assuming its dependency is present;
    // it must not fail the whole ladder just because one runner happens to
    // lack an optional external tool that has nothing to do with Sub0Log
    // itself. A *test* asserting this exact capability would be right to
    // fail here instead -- "git vanished from the CI image" is exactly the
    // kind of regression a test exists to catch, and swallowing it would be
    // a bug in the test, not a courtesy. This is the one place in this file
    // that distinction changes what the code does.
    auto probe = sub0log::ChildProcess::spawn({.argv_ = {"git", "--version"}, .propagateCorrelation_ = false});
    if (!probe.valid()) {
        std::printf("git could not be spawned (%s); skipping this example.\n", probe.error().what_.data());
        return 0;
    }
    const auto probeStatus = probe.wait();
    if (probeStatus.exitCode_ != 0) {
        std::printf("git --version exited %d; treating git as unusable and skipping this example.\n",
                    probeStatus.exitCode_);
        return 0;
    }

    const auto scratch = makeScratchDir("childcapture-git");
    const auto repo = scratch / "throwaway-repo";
    std::filesystem::create_directories(repo);

    auto logger = sub0log::Logger::create({.directory_ = scratch.string(), .stem_ = "gitcapture"});
    if (!logger.valid()) {
        std::fprintf(stderr, "could not create a Logger\n");
        return 1;
    }

    std::string hash1, hash2;
    std::string pathspecError;
    std::vector<std::string> harvestedFullHashes;
    std::uint64_t suppressedTrailers = 0;
    bool sawPathspecFailure = false;

    {
        sub0log::Logger::ScopedBind bind{logger};
        sub0log::CorrelationScope activity{};

        // --- 1. a repository that cannot depend on, or disturb, anything
        //        outside this scratch directory ---------------------------
        runGit(repo, {"init"}, "init");
        // --local is the default inside a repo, said explicitly so this
        // file is not quietly relying on that default -- and, unlike the
        // default, it cannot be shadowed by a --global identity the host
        // running this example happens to have.
        runGit(repo, {"config", "--local", "user.email", "sub0log-example@example.invalid"}, "config user.email");
        runGit(repo, {"config", "--local", "user.name", "Sub0Log Example"}, "config user.name");
        // Local config always wins over global/system for the same key, so
        // this switches signing off for this repository regardless of what
        // the host's own git identity is configured to do -- the whole
        // point of running commits inside a repo nobody else has to trust.
        runGit(repo, {"config", "--local", "commit.gpgsign", "false"}, "config commit.gpgsign");

        // --- 2. two real commits, their hashes harvested as they happen --
        //
        // Every commit message below carries a "Signed-off-by:" trailer on
        // purpose -- real boilerplate `git log --format=full` and friends
        // print by default, and exactly what a log-parsing supervisor
        // filters in step 4. It costs nothing to add and makes the
        // suppression demonstration below real rather than contrived.
        {
            std::ofstream file{repo / "greeting.txt"};
            file << "hello from the git-capture example\n";
        }
        runGit(repo, {"add", "greeting.txt"}, "add (1st)");
        runGit(repo, {"commit", "-m", "add the greeting file", "-m",
                      "Signed-off-by: sub0log-example <sub0log-example@example.invalid>"},
               "commit (1st)", [&](const sub0log::ChildLine& line) {
                   if (const auto hash = harvestShortHash(line.text_)) {
                       hash1 = *hash; // harvested from the summary line, not a second `git rev-parse`
                   }
                   return sub0log::InterceptAction::Log;
               });

        {
            std::ofstream file{repo / "greeting.txt", std::ios::app};
            file << "and a second line, for a second commit\n";
        }
        runGit(repo, {"add", "greeting.txt"}, "add (2nd)");
        runGit(repo, {"commit", "-m", "extend the greeting", "-m",
                      "Signed-off-by: sub0log-example <sub0log-example@example.invalid>"},
               "commit (2nd)", [&](const sub0log::ChildLine& line) {
                   if (const auto hash = harvestShortHash(line.text_)) {
                       hash2 = *hash;
                   }
                   return sub0log::InterceptAction::Log;
               });

        // --- 3. a command made to fail, and the interceptor notices why --
        //
        // The exit code alone says "git failed"; a real supervisor usually
        // wants to know *which* failure it was before deciding what to do
        // about it (retry, alert, ignore). "error: pathspec ... did not
        // match" is git's own wording for "that ref does not exist" and has
        // been stable across git versions for years, which is what makes
        // matching a substring of it safe to pin here.
        runGit(repo, {"checkout", "definitely-not-a-real-branch-xyz"}, "checkout (deliberate failure)",
               [&](const sub0log::ChildLine& line) {
                   if (line.text_.find("pathspec") != std::string_view::npos) {
                       sawPathspecFailure = true;
                       pathspecError = std::string{line.text_};
                   }
                   return sub0log::InterceptAction::Log; // noticed, not hidden -- this is signal
               });

        // --- 4. reading it back, suppressing the boilerplate this time ---
        //
        // Every commit's "Signed-off-by:" trailer is real, useful-to-git,
        // uninteresting-to-a-log-reader boilerplate -- suppressed the same
        // way 06 suppresses "NOISE-" lines, and counted rather than
        // silently gone (R9.1) via the interceptor's own return value.
        // While it is here, the same callback also harvests each commit's
        // *full* hash from `commit <hash>` lines, which is what step 5
        // cross-checks the two short hashes above against, without this
        // file ever hard-coding a hash anywhere.
        runGit(repo, {"log"}, "log", [&](const sub0log::ChildLine& line) {
            if (line.text_.find("Signed-off-by:") != std::string_view::npos) {
                ++suppressedTrailers;
                return sub0log::InterceptAction::Suppress;
            }
            if (line.text_.starts_with("commit ")) {
                const auto rest = line.text_.substr(7);
                const auto space = rest.find(' ');
                harvestedFullHashes.emplace_back(space == std::string_view::npos ? rest : rest.substr(0, space));
            }
            return sub0log::InterceptAction::Log;
        });
    }

    // --- 5. the ledger: what was harvested, noticed, and suppressed ------
    std::printf("\nharvested short hash (1st commit): %s\n", hash1.empty() ? "(none)" : hash1.c_str());
    std::printf("harvested short hash (2nd commit): %s\n", hash2.empty() ? "(none)" : hash2.c_str());
    std::printf("noticed failure: %s\n", sawPathspecFailure ? pathspecError.c_str() : "(none -- unexpected)");
    std::printf("suppressed \"Signed-off-by:\" trailers: %llu\n\n",
                static_cast<unsigned long long>(suppressedTrailers));

    bool ok = true;
    ok = ok && !hash1.empty() && !hash2.empty() && hash1 != hash2;
    ok = ok && sawPathspecFailure;
    ok = ok && suppressedTrailers == 2u;
    ok = ok && harvestedFullHashes.size() == 2u;
    // git log lists newest first: the 2nd commit's full hash, then the
    // 1st's -- each should begin with the short hash harvested from its
    // own `git commit` summary line above. This is the determinism check:
    // nothing here is a hard-coded hash, only an internal consistency
    // check between two independent readings of the same repository.
    if (ok) {
        ok = ok && harvestedFullHashes[0].starts_with(hash2);
        ok = ok && harvestedFullHashes[1].starts_with(hash1);
    }

    // --- 6. and it is all durable, attributed, and decodable, same as 06 -
    const auto image = slurp(onlySegment(scratch));
    auto reader = sub0log::SegmentReader::open(image);
    if (!reader.valid()) {
        std::fprintf(stderr, "segment did not open\n");
        return 1;
    }
    std::size_t starts = 0, exits = 0;
    reader.visit([&](const sub0log::RecordView& v) {
        using sub0log::wire::RecordKind;
        if (v.head_.kind_ == RecordKind::ChildStart) {
            ++starts;
        } else if (v.head_.kind_ == RecordKind::ChildExit) {
            ++exits;
        }
    });
    // One ChildStart/ChildExit pair per `git` invocation above: init, three
    // config calls, two add, two commit, one checkout, one log -- ten. (The
    // `git --version` probe in step 0 ran before any Logger existed, so it
    // wrote nothing here -- see its own comment for why.)
    std::printf("raw segment: %zu ChildStart record(s), %zu ChildExit record(s)\n", starts, exits);
    ok = ok && (starts == 10u && exits == 10u);

    std::filesystem::remove_all(scratch);
    if (!ok) {
        std::fprintf(stderr, "one or more expectations about the git capture did not hold\n");
        return 1;
    }
    std::printf("example 10 OK\n");
    return 0;
}
