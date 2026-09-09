#include "device_control_scheduler.h"
#include "device_command_service.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "unity.h"

static volatile uint32_t s_sends, s_done;
static gw_message_t s_last;
static device_control_result_t s_last_result;
static int mock_send(const char *id, const gw_message_t *message) { (void)id; s_last=*message; s_sends++; return 0; }
static int mock_connected(const char *id) { (void)id; return 1; }
static const device_command_transport_hooks_t s_hooks={.send_command=mock_send,.is_connected=mock_connected};
static void done(const device_control_result_t *result, void *ctx) { (void)ctx; s_last_result=*result; s_done++; }
static device_control_job_t job(const char *id, device_control_priority_t priority) { device_control_job_t j={0};j.priority=priority;j.source=DEVICE_CTRL_SOURCE_INTERNAL;j.request.origin=DEVICE_CMD_ORIGIN_SCHEMA_DISCOVERY;strlcpy(j.request.device_id,id,sizeof(j.request.device_id));strlcpy(j.request.command,"describe_capabilities",sizeof(j.request.command));return j; }
static void wait_ms(uint32_t ms){vTaskDelay(pdMS_TO_TICKS(ms));}
static void ack(void){gw_message_t a={0};strlcpy(a.type,"device_ack",sizeof(a.type));strlcpy(a.command,"describe_capabilities",sizeof(a.command));a.has_request_id=1;a.request_id=s_last.request_id;a.bool_value=1;TEST_ASSERT_TRUE(device_command_service_on_notify(s_last.device_id,&a));}
static void setup(void){s_sends=s_done=0;memset(&s_last,0,sizeof(s_last));device_command_service_set_hooks(&s_hooks);TEST_ASSERT_EQUAL(ESP_OK,device_command_service_init());TEST_ASSERT_EQUAL(ESP_OK,device_control_scheduler_init());}
static void teardown(void){device_control_scheduler_deinit();device_command_service_deinit();}

TEST_CASE("scheduler serializes same-device transport", "[device_control_scheduler]")
{ setup(); device_control_job_t a=job("a",DEVICE_CTRL_PRIORITY_NORMAL),b=job("a",DEVICE_CTRL_PRIORITY_NORMAL);TEST_ASSERT_EQUAL(ESP_OK,device_control_scheduler_submit(&a,done,NULL,NULL));TEST_ASSERT_EQUAL(ESP_OK,device_control_scheduler_submit(&b,done,NULL,NULL));wait_ms(30);TEST_ASSERT_EQUAL_UINT32(1,s_sends);ack();wait_ms(30);TEST_ASSERT_EQUAL_UINT32(2,s_sends);ack();wait_ms(30);TEST_ASSERT_EQUAL_UINT32(2,s_done);teardown(); }

TEST_CASE("scheduler bounds global inflight to four", "[device_control_scheduler]")
{ setup(); for(int i=0;i<5;i++){char id[4];snprintf(id,sizeof(id),"d%d",i);device_control_job_t j=job(id,DEVICE_CTRL_PRIORITY_NORMAL);TEST_ASSERT_EQUAL(ESP_OK,device_control_scheduler_submit(&j,done,NULL,NULL));}wait_ms(120);device_control_scheduler_stats_t stats={0};device_control_scheduler_get_stats(&stats);TEST_ASSERT_EQUAL_UINT32(4,stats.max_inflight);ack();wait_ms(250);device_control_scheduler_get_stats(&stats);TEST_ASSERT_EQUAL_UINT32(5,stats.dispatched);teardown(); }

TEST_CASE("scheduler expires queue deadline exactly once", "[device_control_scheduler]")
{ setup();TEST_ASSERT_EQUAL(ESP_OK,device_control_scheduler_block_device("deadline"));device_control_job_t j=job("deadline",DEVICE_CTRL_PRIORITY_NORMAL);j.max_queue_wait_ms=20;TEST_ASSERT_EQUAL(ESP_OK,device_control_scheduler_submit(&j,done,NULL,NULL));wait_ms(60);TEST_ASSERT_EQUAL_UINT32(0,s_sends);TEST_ASSERT_EQUAL_UINT32(1,s_done);TEST_ASSERT_EQUAL(DEVICE_CTRL_TERMINAL_DEADLINE_EXCEEDED,s_last_result.terminal);teardown(); }

static device_control_lease_t s_lease; static void lease_done(esp_err_t e,device_control_lease_t id,void*ctx){(void)ctx;TEST_ASSERT_EQUAL(ESP_OK,e);s_lease=id;}
TEST_CASE("lease blocks non-owner and release unblocks", "[device_control_scheduler]")
{ setup();s_lease=0;device_control_lease_request_t r={.device_id="lease",.priority=DEVICE_CTRL_PRIORITY_NORMAL,.source=DEVICE_CTRL_SOURCE_SETTINGS,.owner_token=7};TEST_ASSERT_EQUAL(ESP_OK,device_control_scheduler_acquire_lease(&r,lease_done,NULL));wait_ms(30);TEST_ASSERT_NOT_EQUAL(0,s_lease);device_control_job_t owned=job("lease",DEVICE_CTRL_PRIORITY_NORMAL);owned.lease_id=s_lease;device_control_job_t web=job("lease",DEVICE_CTRL_PRIORITY_HIGH);web.source=DEVICE_CTRL_SOURCE_WEB;TEST_ASSERT_EQUAL(ESP_OK,device_control_scheduler_submit(&owned,done,NULL,NULL));TEST_ASSERT_EQUAL(ESP_OK,device_control_scheduler_submit(&web,done,NULL,NULL));wait_ms(30);TEST_ASSERT_EQUAL_UINT32(1,s_sends);ack();wait_ms(30);TEST_ASSERT_EQUAL_UINT32(1,s_sends);TEST_ASSERT_EQUAL(ESP_OK,device_control_scheduler_release_lease(s_lease));wait_ms(30);TEST_ASSERT_EQUAL_UINT32(2,s_sends);teardown(); }

TEST_CASE("targeted source cancel leaves other work queued", "[device_control_scheduler]")
{ setup();TEST_ASSERT_EQUAL(ESP_OK,device_control_scheduler_block_device("cancel"));device_control_job_t state=job("cancel",DEVICE_CTRL_PRIORITY_BACKGROUND);state.source=DEVICE_CTRL_SOURCE_STATE;state.owner_token=1;device_control_job_t settings=job("cancel",DEVICE_CTRL_PRIORITY_NORMAL);settings.source=DEVICE_CTRL_SOURCE_SETTINGS;TEST_ASSERT_EQUAL(ESP_OK,device_control_scheduler_submit(&state,done,NULL,NULL));TEST_ASSERT_EQUAL(ESP_OK,device_control_scheduler_submit(&settings,done,NULL,NULL));TEST_ASSERT_EQUAL(ESP_OK,device_control_scheduler_cancel_source("cancel",DEVICE_CTRL_SOURCE_STATE,1));wait_ms(30);TEST_ASSERT_EQUAL_UINT32(1,s_done);TEST_ASSERT_EQUAL(DEVICE_CTRL_TERMINAL_CANCELLED,s_last_result.terminal);TEST_ASSERT_EQUAL(ESP_OK,device_control_scheduler_unblock_device("cancel"));wait_ms(30);TEST_ASSERT_EQUAL_UINT32(1,s_sends);teardown(); }

TEST_CASE("scheduler prefers high work at dispatch boundary", "[device_control_scheduler]")
{ setup();TEST_ASSERT_EQUAL(ESP_OK,device_control_scheduler_block_device("priority"));device_control_job_t background=job("priority",DEVICE_CTRL_PRIORITY_BACKGROUND),high=job("priority",DEVICE_CTRL_PRIORITY_HIGH);TEST_ASSERT_EQUAL(ESP_OK,device_control_scheduler_submit(&background,done,NULL,NULL));TEST_ASSERT_EQUAL(ESP_OK,device_control_scheduler_submit(&high,done,NULL,NULL));TEST_ASSERT_EQUAL(ESP_OK,device_control_scheduler_unblock_device("priority"));wait_ms(40);TEST_ASSERT_EQUAL_UINT32(1,s_sends);ack();wait_ms(40);TEST_ASSERT_EQUAL_UINT32(2,s_sends);teardown(); }

TEST_CASE("background dedupe replacement supersedes queued work", "[device_control_scheduler]")
{ setup();TEST_ASSERT_EQUAL(ESP_OK,device_control_scheduler_block_device("dedupe"));device_control_job_t old=job("dedupe",DEVICE_CTRL_PRIORITY_BACKGROUND),replacement=old;old.dedupe=DEVICE_CTRL_DEDUPE_REPLACE_QUEUED;replacement.dedupe=DEVICE_CTRL_DEDUPE_REPLACE_QUEUED;old.dedupe_hash=replacement.dedupe_hash=0xA11;TEST_ASSERT_EQUAL(ESP_OK,device_control_scheduler_submit(&old,done,NULL,NULL));TEST_ASSERT_EQUAL(ESP_OK,device_control_scheduler_submit(&replacement,done,NULL,NULL));wait_ms(40);TEST_ASSERT_EQUAL_UINT32(1,s_done);TEST_ASSERT_EQUAL(DEVICE_CTRL_TERMINAL_SUPERSEDED,s_last_result.terminal);TEST_ASSERT_EQUAL(ESP_OK,device_control_scheduler_unblock_device("dedupe"));wait_ms(40);TEST_ASSERT_EQUAL_UINT32(1,s_sends);teardown(); }

TEST_CASE("quiesce cancels queued work and leaves device idle", "[device_control_scheduler]")
{ setup();TEST_ASSERT_EQUAL(ESP_OK,device_control_scheduler_block_device("quiet"));device_control_job_t queued=job("quiet",DEVICE_CTRL_PRIORITY_NORMAL);TEST_ASSERT_EQUAL(ESP_OK,device_control_scheduler_submit(&queued,done,NULL,NULL));TEST_ASSERT_EQUAL(ESP_OK,device_control_scheduler_quiesce_device("quiet",200));wait_ms(30);TEST_ASSERT_EQUAL_UINT32(1,s_done);TEST_ASSERT_TRUE(device_control_scheduler_is_idle("quiet"));teardown(); }
