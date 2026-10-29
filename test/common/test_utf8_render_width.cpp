#include "catch.hpp"
#include "utf8proc_wrapper.hpp"

#include <string>

using namespace duckdb;

// the width of the first codepoint of every grapheme cluster
static size_t ClusterWidth(const std::string &str) {
	size_t width = 0;
	for (auto cluster : Utf8Proc::GraphemeClusters(str.c_str(), str.size())) {
		width += Utf8Proc::RenderWidth(str.c_str(), str.size(), cluster.start);
	}
	return width;
}

TEST_CASE("RenderWidth of ASCII matches the width of its grapheme clusters", "[utf8]") {
	std::string all;
	for (int c = 0; c < 0x80; c++) {
		std::string single(1, static_cast<char>(c));
		REQUIRE(Utf8Proc::RenderWidth(single) == ClusterWidth(single));
		all += single;
	}
	REQUIRE(Utf8Proc::RenderWidth(all) == 95);
	REQUIRE(Utf8Proc::RenderWidth(all) == ClusterWidth(all));
	for (std::string str : {"", "a\r\nb", "\t x\x7f"}) {
		REQUIRE(Utf8Proc::RenderWidth(str) == ClusterWidth(str));
	}
}

TEST_CASE("RenderWidth of ASCII next to other characters", "[utf8]") {
	// a combining accent, a wide character and an emoji sequence after ASCII
	REQUIRE(Utf8Proc::RenderWidth("ae\xcc\x81") == 2);
	REQUIRE(Utf8Proc::RenderWidth("a\xe6\x97\xa5") == 3);
	REQUIRE(Utf8Proc::RenderWidth("\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb a") == 4);
}
