#include <pthread.h>
#include <unistd.h>
#include <CppUTest/CommandLineTestRunner.h>
#include <CppUTest/TestHarness.h>
#include <latency/client.h>
#include <latency/server.h>

void *run_latency_server(void *dummy) {
  char ip[] = "127.0.0.1";
  int port = 1234;
  uint64_t size = 4096;
  if (serve(ip, port, size) == -1) {
    abort();
  }
}

TEST_GROUP(TestLatencyGroup) {
  void setup() {}
  void teardown() {}
};

TEST(TestLatencyGroup, TestClientServer) {
  pthread_t server;
  pthread_create(&server, NULL, run_latency_server, NULL);
  char ip[] = "127.0.0.1";
  int port = 1234;
  usleep(500000);
  CHECK_EQUAL(benchmark(ip, port, 10, 10, 4096), 0);
}

int main(int ac, char** av)
{
  MemoryLeakWarningPlugin::turnOffNewDeleteOverloads();
  return CommandLineTestRunner::RunAllTests(ac, av);
}
