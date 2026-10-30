#include "results/provenance.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "fixtures.hpp"

namespace aadp::results {
namespace {

namespace fs = std::filesystem;

fs::path fresh_dir(std::string_view name) {
  const fs::path dir = fs::path(AADP_TEST_SCRATCH) / name;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

TEST(Provenance, ParsesCommitFile) {
  const std::string hash(testing::kCommit);
  EXPECT_EQ(parse_commit_file(hash + "\n"), hash);
  EXPECT_EQ(parse_commit_file(hash), hash);
  EXPECT_FALSE(parse_commit_file("").has_value());
  EXPECT_FALSE(parse_commit_file("deadbeef\n").has_value());
  EXPECT_FALSE(parse_commit_file(hash + "\nextra\n").has_value());
}

TEST(Provenance, AnExportIsReadFromItsCommitFile) {
  const fs::path dir = fresh_dir("export");
  std::ofstream(dir / "COMMIT") << testing::kCommit << "\n";
  const Provenance found = read_provenance(dir);
  EXPECT_EQ(found.commit, testing::kCommit);
  EXPECT_EQ(found.source, "COMMIT file");
  EXPECT_EQ(found.tree_state, "exported");
}

// The export sits inside the repo's build tree. Asking git would name the enclosing repo.
TEST(Provenance, NoCommitFileMeansUnknownEvenInsideARepo) {
  const fs::path dir = fresh_dir("nothing");
  const Provenance found = read_provenance(dir);
  EXPECT_EQ(found.commit, "unknown");
  EXPECT_EQ(found.tree_state, "unknown");
}

TEST(Provenance, AGarbledCommitFileMeansUnknown) {
  const fs::path dir = fresh_dir("garbled");
  std::ofstream(dir / "COMMIT") << "not a hash\n";
  EXPECT_EQ(read_provenance(dir).tree_state, "unknown");
}

TEST(Provenance, GitCheckoutReportsCleanThenDirty) {
  const fs::path dir = fresh_dir("repo");
  const std::string git = "git -C '" + dir.string() + "' ";
  const std::string identity = "-c user.name=t -c user.email=t@example.invalid ";
  ASSERT_EQ(std::system((git + "init -q").c_str()), 0);
  std::ofstream(dir / "file.txt") << "one\n";
  ASSERT_EQ(std::system((git + "add file.txt").c_str()), 0);
  ASSERT_EQ(std::system((git + identity + "commit -q -m first").c_str()), 0);

  const Provenance clean = read_provenance(dir);
  EXPECT_EQ(clean.source, "git");
  EXPECT_EQ(clean.tree_state, "clean");
  EXPECT_TRUE(is_commit_hash(clean.commit));

  std::ofstream(dir / "file.txt") << "two\n";
  EXPECT_EQ(read_provenance(dir).tree_state, "dirty");

  ASSERT_EQ(std::system((git + "checkout -q file.txt").c_str()), 0);
  EXPECT_EQ(read_provenance(dir).tree_state, "clean");
  std::ofstream(dir / "untracked.txt") << "new\n";
  EXPECT_EQ(read_provenance(dir).tree_state, "dirty");
}

}  // namespace
}  // namespace aadp::results
