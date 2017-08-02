#include <pthread.h>
#include <unistd.h>
#include <CppUTest/CommandLineTestRunner.h>
#include <CppUTest/TestHarness.h>
#include <latency/client.h>
#include <latency/server.h>
#include <latency/scheduler.h>

TEST_GROUP(TestLatencyGroup) {
  void setup() {}
  void teardown() {}
};

TEST(TestLatencyGroup, TestClientServer) {
  const char ip[] = "127.0.0.1";
  const int port = 1234;
  ssize_t size = 4096;
  int count = 10;
  int samples = 100;
  pthread_t server;
  pthread_t client;
  struct server_args s = {
    .ip = ip,
    .port = port,
    .size = size,
    .rcpu = 1,
    .wcpu = 0,
  };
  struct client_args c = {
    .ip = ip,
    .size = size,
    .port = port,
    .count = count,
    .samples = samples,
    .rcpu = 2,
    .wcpu = 3,
  };
  void *sret = NULL;
  void *cret = NULL;
  run_server(&server, &s);
  usleep(500000);
  run_client(&client, &c);
  pthread_join(client, &cret);
  //CHECK_EQUAL((uint64_t)cret, 0);
  kill_server(&server, &s, &sret);
}

int main(int ac, char** av)
{
  MemoryLeakWarningPlugin::turnOffNewDeleteOverloads();
  return CommandLineTestRunner::RunAllTests(ac, av);
}
