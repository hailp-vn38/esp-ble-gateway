#include <stdio.h>

#include "log_buffer.h"
#include "unity.h"

TEST_CASE("log buffer returns only the most recent requested entries", "[log_buffer]")
{
    log_buffer_init();
    for (int i = 0; i < 30; i++) {
        char text[24];
        snprintf(text, sizeof(text), "entry-%02d", i);
        log_buffer_push(text);
    }

    log_entry_t recent[5];
    TEST_ASSERT_EQUAL_INT(5, log_buffer_get_recent(recent, 5));
    TEST_ASSERT_EQUAL_STRING("entry-25", recent[0].text);
    TEST_ASSERT_EQUAL_STRING("entry-29", recent[4].text);
}

TEST_CASE("log buffer recent API validates its limit", "[log_buffer]")
{
    log_entry_t entry;
    TEST_ASSERT_EQUAL_INT(-1, log_buffer_get_recent(&entry, 0));
    TEST_ASSERT_EQUAL_INT(-1, log_buffer_get_recent(NULL, 1));
}
