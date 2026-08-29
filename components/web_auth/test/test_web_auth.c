#include "unity.h"
#include "esp_timer.h"
#include "nvs.h"
#include "web_auth.h"
#include "web_auth_password.h"
#include "web_auth_session.h"
#include "web_auth_store.h"

#include <string.h>

static void reset_auth_store(void)
{
    esp_err_t err = web_auth_store_erase();
    TEST_ASSERT_TRUE(err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND);
    TEST_ASSERT_EQUAL(ESP_OK, web_auth_init());
    web_auth_invalidate_all_sessions();
}

TEST_CASE("web_auth_init succeeds", "[web_auth]")
{
    reset_auth_store();
    esp_err_t err = web_auth_init();
    TEST_ASSERT_EQUAL(ESP_OK, err);
}

TEST_CASE("web_auth_get_status returns default state", "[web_auth]")
{
    reset_auth_store();
    web_auth_status_t status;
    esp_err_t err = web_auth_get_status(&status);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_FALSE(status.enabled);
    TEST_ASSERT_FALSE(status.credentials_configured);
}

TEST_CASE("username validation", "[web_auth]")
{
    // Valid usernames
    TEST_ASSERT_TRUE(web_auth_username_validate("admin"));
    TEST_ASSERT_TRUE(web_auth_username_validate("user123"));
    TEST_ASSERT_TRUE(web_auth_username_validate("test-user"));
    TEST_ASSERT_TRUE(web_auth_username_validate("a.b_c"));

    // Invalid usernames
    TEST_ASSERT_FALSE(web_auth_username_validate(NULL));
    TEST_ASSERT_FALSE(web_auth_username_validate("ab"));  // Too short
    TEST_ASSERT_FALSE(web_auth_username_validate("a"));   // Too short
    TEST_ASSERT_FALSE(web_auth_username_validate("this username is way too long for validation"));
    TEST_ASSERT_FALSE(web_auth_username_validate("user name"));  // Space
    TEST_ASSERT_FALSE(web_auth_username_validate("user@name"));  // Invalid char
}

TEST_CASE("password validation", "[web_auth]")
{
    // Valid passwords
    TEST_ASSERT_TRUE(web_auth_password_validate("password123"));
    TEST_ASSERT_TRUE(web_auth_password_validate("12345678"));
    TEST_ASSERT_TRUE(web_auth_password_validate("a]verylongpasswordwithspecialchars!@#$%"));

    // Invalid passwords
    TEST_ASSERT_FALSE(web_auth_password_validate(NULL));
    TEST_ASSERT_FALSE(web_auth_password_validate("short"));  // Too short
    TEST_ASSERT_FALSE(web_auth_password_validate("1234567"));  // 7 chars, too short
}

TEST_CASE("PBKDF2 SHA-256 matches standard vector", "[web_auth]")
{
    static const uint8_t expected[32] = {
        0x5e, 0xc0, 0x2b, 0x91, 0xa4, 0xb5, 0x9c, 0x6f,
        0x59, 0xdd, 0x5f, 0xbe, 0x4c, 0xa6, 0x49, 0xec,
        0xe4, 0xfa, 0x85, 0x68, 0xcd, 0xb8, 0xba, 0x36,
        0xcf, 0x41, 0x42, 0x6e, 0x88, 0x05, 0x52, 0x2b,
    };
    uint8_t output[32];
    TEST_ASSERT_EQUAL(
        ESP_OK,
        web_auth_password_derive("password", (const uint8_t *)"salt", 4,
                                 10000, output, sizeof(output)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, output, sizeof(output));
}

TEST_CASE("PBKDF2 accepts password lengths 8 32 and 64", "[web_auth]")
{
    static const char password_32[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    static const char password_64[] =
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    static const char *passwords[] = {
        "12345678", password_32, password_64,
    };
    uint8_t output[32];
    for (size_t i = 0; i < sizeof(passwords) / sizeof(passwords[0]); i++) {
        memset(output, 0, sizeof(output));
        TEST_ASSERT_EQUAL(
            ESP_OK,
            web_auth_password_derive(passwords[i], (const uint8_t *)"salt",
                                     4, 10000, output, sizeof(output)));
        TEST_ASSERT_NOT_EQUAL(0, output[0] | output[1] | output[2] | output[3]);
    }
}

TEST_CASE("PBKDF2 benchmark 10k 30k 60k", "[web_auth][benchmark]")
{
    static const uint32_t costs[] = {10000, 30000, 60000};
    uint8_t output[32];

    for (size_t cost_index = 0;
         cost_index < sizeof(costs) / sizeof(costs[0]); cost_index++) {
        int64_t samples_us[3];
        for (size_t sample = 0; sample < 3; sample++) {
            int64_t started_us = esp_timer_get_time();
            TEST_ASSERT_EQUAL(
                ESP_OK,
                web_auth_password_derive(
                    "benchmark-password", (const uint8_t *)"benchmark-salt",
                    14, costs[cost_index], output, sizeof(output)));
            samples_us[sample] = esp_timer_get_time() - started_us;
            printf("BENCH_PBKDF2 iterations=%lu sample=%u elapsed_us=%lld\n",
                   (unsigned long)costs[cost_index], (unsigned)(sample + 1),
                   (long long)samples_us[sample]);
        }

        if (samples_us[0] > samples_us[1]) {
            int64_t swap = samples_us[0];
            samples_us[0] = samples_us[1];
            samples_us[1] = swap;
        }
        if (samples_us[1] > samples_us[2]) {
            int64_t swap = samples_us[1];
            samples_us[1] = samples_us[2];
            samples_us[2] = swap;
        }
        if (samples_us[0] > samples_us[1]) {
            int64_t swap = samples_us[0];
            samples_us[0] = samples_us[1];
            samples_us[1] = swap;
        }
        printf("BENCH_PBKDF2_MEDIAN iterations=%lu elapsed_us=%lld\n",
               (unsigned long)costs[cost_index],
               (long long)samples_us[1]);
    }
}

TEST_CASE("session token round trip and logout", "[web_auth]")
{
    TEST_ASSERT_EQUAL(ESP_OK, web_auth_session_init());
    web_auth_session_destroy_all();

    char token[WEB_AUTH_SESSION_TOKEN_BUFFER_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK,
                      web_auth_session_create(token, sizeof(token)));
    TEST_ASSERT_EQUAL(WEB_AUTH_SESSION_TOKEN_LENGTH, strlen(token));
    TEST_ASSERT_EQUAL(WEB_AUTH_OK, web_auth_session_validate(token));

    char changed[WEB_AUTH_SESSION_TOKEN_BUFFER_SIZE];
    strlcpy(changed, token, sizeof(changed));
    changed[0] = changed[0] == 'A' ? 'B' : 'A';
    TEST_ASSERT_EQUAL(WEB_AUTH_INVALID_CREDENTIALS,
                      web_auth_session_validate(changed));
    changed[0] = '!';
    TEST_ASSERT_EQUAL(WEB_AUTH_INVALID_CREDENTIALS,
                      web_auth_session_validate(changed));

    web_auth_session_destroy(token);
    TEST_ASSERT_EQUAL(WEB_AUTH_INVALID_CREDENTIALS,
                      web_auth_session_validate(token));
}

TEST_CASE("logout only destroys the selected session", "[web_auth]")
{
    TEST_ASSERT_EQUAL(ESP_OK, web_auth_session_init());
    web_auth_session_destroy_all();

    char first[WEB_AUTH_SESSION_TOKEN_BUFFER_SIZE];
    char second[WEB_AUTH_SESSION_TOKEN_BUFFER_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK,
                      web_auth_session_create(first, sizeof(first)));
    TEST_ASSERT_EQUAL(ESP_OK,
                      web_auth_session_create(second, sizeof(second)));
    web_auth_session_destroy(first);
    TEST_ASSERT_EQUAL(WEB_AUTH_INVALID_CREDENTIALS,
                      web_auth_session_validate(first));
    TEST_ASSERT_EQUAL(WEB_AUTH_OK, web_auth_session_validate(second));
}

TEST_CASE("login session validates without reloading credentials", "[web_auth]")
{
    reset_auth_store();
    TEST_ASSERT_EQUAL(WEB_AUTH_OK,
                      web_auth_enable("admin", NULL, "password123"));

    char token[WEB_AUTH_SESSION_TOKEN_BUFFER_SIZE];
    TEST_ASSERT_EQUAL(WEB_AUTH_OK,
                      web_auth_login("admin", "password123", token,
                                     sizeof(token)));
    TEST_ASSERT_EQUAL(WEB_AUTH_OK, web_auth_validate_session(token));
    web_auth_logout(token);
    TEST_ASSERT_EQUAL(WEB_AUTH_INVALID_CREDENTIALS,
                      web_auth_validate_session(token));
}
