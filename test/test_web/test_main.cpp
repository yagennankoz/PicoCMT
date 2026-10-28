// ============================================================================
// Webファイルマネージャの文字列処理 (cmt_web_util) のテスト (pio test -e native)
// ============================================================================
#include <unity.h>

#include <stdint.h>
#include <string.h>

#include <string>

#include "cmt_web_util.h"

// 一緒にリンクされるファームウェア側のコードが参照するグローバル
volatile uint32_t playback_base_time_ms = 0;
volatile uint32_t playback_tag_start_system_ms = 0;
volatile bool playback_time_valid = false;
void draw_conversion_progress(int) {}

static std::string normalize(const char* in) {
    char out[64];
    return web_normalize_path(in, out, sizeof(out)) ? std::string(out) : std::string("<NG>");
}

void test_normalize_path() {
    TEST_ASSERT_EQUAL_STRING("/", normalize("/").c_str());
    TEST_ASSERT_EQUAL_STRING("/a/b", normalize("/a/b").c_str());
    TEST_ASSERT_EQUAL_STRING("/a/b", normalize("//a///b/").c_str());
    TEST_ASSERT_EQUAL_STRING("/a/b", normalize("/a/./b/.").c_str());
    TEST_ASSERT_EQUAL_STRING("/a..b/c", normalize("/a..b/c").c_str());  // ".." を含む名前はよい
    TEST_ASSERT_EQUAL_STRING("/GAME 1.T88", normalize("/GAME 1.T88").c_str());
}

void test_normalize_path_rejects_unsafe_input() {
    TEST_ASSERT_EQUAL_STRING("<NG>", normalize("").c_str());
    TEST_ASSERT_EQUAL_STRING("<NG>", normalize("a/b").c_str());       // 相対パス
    TEST_ASSERT_EQUAL_STRING("<NG>", normalize("/..").c_str());       // 上の階層へ出る
    TEST_ASSERT_EQUAL_STRING("<NG>", normalize("/a/../b").c_str());
    TEST_ASSERT_EQUAL_STRING("<NG>", normalize("/a\\b").c_str());     // バックスラッシュ
    TEST_ASSERT_EQUAL_STRING("<NG>", normalize("/a\nb").c_str());     // 制御文字
    TEST_ASSERT_FALSE(web_normalize_path(nullptr, nullptr, 0));

    // 出力先に入りきらない
    char small[4];
    TEST_ASSERT_TRUE(web_normalize_path("/ab", small, sizeof(small)));
    TEST_ASSERT_FALSE(web_normalize_path("/abc", small, sizeof(small)));
}

void test_valid_name() {
    TEST_ASSERT_TRUE(web_is_valid_name("GAME.T88"));
    TEST_ASSERT_TRUE(web_is_valid_name("ゲーム.T77"));  // UTF-8
    TEST_ASSERT_TRUE(web_is_valid_name(".hidden"));
    TEST_ASSERT_FALSE(web_is_valid_name(""));
    TEST_ASSERT_FALSE(web_is_valid_name("."));
    TEST_ASSERT_FALSE(web_is_valid_name(".."));
    TEST_ASSERT_FALSE(web_is_valid_name("a/b"));
    TEST_ASSERT_FALSE(web_is_valid_name("a\\b"));
    TEST_ASSERT_FALSE(web_is_valid_name("a:b"));
    TEST_ASSERT_FALSE(web_is_valid_name("a*b"));
    TEST_ASSERT_FALSE(web_is_valid_name("a?b"));
    TEST_ASSERT_FALSE(web_is_valid_name("a\"b"));
    TEST_ASSERT_FALSE(web_is_valid_name("a<b>"));
    TEST_ASSERT_FALSE(web_is_valid_name("a|b"));
    TEST_ASSERT_FALSE(web_is_valid_name("a\tb"));
    TEST_ASSERT_FALSE(web_is_valid_name("name."));  // FAT では末尾のピリオド・空白は不可
    TEST_ASSERT_FALSE(web_is_valid_name("name "));
    TEST_ASSERT_FALSE(web_is_valid_name(std::string(101, 'a').c_str()));
    TEST_ASSERT_TRUE(web_is_valid_name(std::string(100, 'a').c_str()));
}

void test_join_and_parent_path() {
    char out[32];
    TEST_ASSERT_TRUE(web_join_path("/", "A.T88", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("/A.T88", out);
    TEST_ASSERT_TRUE(web_join_path("/dir", "A.T88", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("/dir/A.T88", out);
    TEST_ASSERT_FALSE(web_join_path("/dir", "0123456789012345678901234567", out, sizeof(out)));

    TEST_ASSERT_TRUE(web_parent_path("/a/b", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("/a", out);
    TEST_ASSERT_TRUE(web_parent_path("/a", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("/", out);
    TEST_ASSERT_FALSE(web_parent_path("/", out, sizeof(out)));
}

void test_path_is_within() {
    TEST_ASSERT_TRUE(web_path_is_within("/a", "/a"));
    TEST_ASSERT_TRUE(web_path_is_within("/a/b", "/a"));
    TEST_ASSERT_FALSE(web_path_is_within("/ab", "/a"));  // 名前が前方一致するだけのものは別
    TEST_ASSERT_FALSE(web_path_is_within("/b", "/a"));
    TEST_ASSERT_TRUE(web_path_is_within("/x", "/"));
}

void test_base_name() {
    TEST_ASSERT_EQUAL_STRING("A.T88", web_base_name("A.T88"));
    TEST_ASSERT_EQUAL_STRING("A.T88", web_base_name("/dir/A.T88"));
    TEST_ASSERT_EQUAL_STRING("A.T88", web_base_name("C:\\tapes\\A.T88"));
}

void test_json_escape() {
    char out[64];
    TEST_ASSERT_EQUAL_UINT32(5, web_json_escape("A.T88", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("A.T88", out);
    web_json_escape("a\"b\\c", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("a\\\"b\\\\c", out);
    web_json_escape("a\nb\x01", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("a\\u000ab\\u0001", out);
    web_json_escape("ゲーム", out, sizeof(out));  // UTF-8 はそのまま
    TEST_ASSERT_EQUAL_STRING("ゲーム", out);

    char small[4];
    TEST_ASSERT_EQUAL_UINT32(3, web_json_escape("abc", small, sizeof(small)));
    TEST_ASSERT_EQUAL_UINT32((size_t)-1, web_json_escape("abcd", small, sizeof(small)));
    TEST_ASSERT_EQUAL_UINT32((size_t)-1, web_json_escape("a\"b", small, sizeof(small)));
}

void test_url_encode() {
    char out[64];
    web_url_encode("GAME-1_a.b~", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("GAME-1_a.b~", out);
    web_url_encode("a b+c", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("a%20b%2Bc", out);
    web_url_encode("\xE3\x81\x82", out, sizeof(out));  // "あ"
    TEST_ASSERT_EQUAL_STRING("%E3%81%82", out);

    char small[4];
    TEST_ASSERT_EQUAL_UINT32((size_t)-1, web_url_encode("a b", small, sizeof(small)));
}

void setUp() {}
void tearDown() {}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_normalize_path);
    RUN_TEST(test_normalize_path_rejects_unsafe_input);
    RUN_TEST(test_valid_name);
    RUN_TEST(test_join_and_parent_path);
    RUN_TEST(test_path_is_within);
    RUN_TEST(test_base_name);
    RUN_TEST(test_json_escape);
    RUN_TEST(test_url_encode);
    return UNITY_END();
}
