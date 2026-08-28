#include "unity.h"
#include "web_auth.h"

void setUp(void) {}
void tearDown(void) {}

TEST_CASE("web_auth_init succeeds", "[web_auth]")
{
    esp_err_t err = web_auth_init();
    TEST_ASSERT_EQUAL(ESP_OK, err);
}

TEST_CASE("web_auth_get_status returns default state", "[web_auth]")
{
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
