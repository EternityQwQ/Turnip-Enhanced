#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <gtest/gtest.h>

#ifdef _WIN32
#include <process.h>
#define test_getpid _getpid
#else
#include <unistd.h>
#define test_getpid getpid
#endif

#include "c11/threads.h"
#include "util/perf/u_trace.h"
#include "util/os_misc.h"

#if U_TRACE_HAS_COMPRESS
#include <zlib.h>
#endif

#define NUM_DEBUG_TEST_THREAD 8

static const char *trace_file_path =
   "tracefile_for_test-b5ba5a0c-6ed1-4901-a38d-755991182663";

static std::string
trace_file_with(const char *suffix)
{
   return std::string(trace_file_path) + suffix;
}

/* u_trace reads its MESA_GPU_TRACE* options once, so the state is reset
 * around each test to let every case set its own.
 */
class UtilPerfTraceTest : public ::testing::Test {
protected:
   void SetUp() override
   {
      u_trace_state_reset();
   }

   void TearDown() override
   {
      u_trace_state_reset();
      remove(trace_file_path);
   }
};

static void
init_context(struct u_trace_context *ctx)
{
   memset(ctx, 0, sizeof(*ctx));
   u_trace_context_init(ctx, NULL, 8, 0, NULL, NULL, NULL,
                        NULL, NULL, NULL, NULL);
}

/* Opens and closes one frame with an empty batch, which needs no timestamp
 * callbacks, and waits for it to be printed.
 */
static void
process_frame(struct u_trace_context *ctx)
{
   struct u_trace ut;
   u_trace_init(&ut, ctx);
   u_trace_flush(&ut, NULL, 0, false);
   u_trace_fini(&ut);
   u_trace_context_process(ctx, true);
   util_queue_finish(&ctx->queue);
}

static std::string
read_file(const char *path)
{
   std::string contents;
   FILE *f = fopen(path, "rb");
   if (!f)
      return contents;

   char buf[256];
   size_t n;
   while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
      contents.append(buf, n);
   fclose(f);
   return contents;
}

#if U_TRACE_HAS_COMPRESS
/* complete is cleared if the stream ends without its trailer, i.e. it was
 * never closed.
 */
static std::string
read_gz_file(const char *path, bool *complete)
{
   std::string contents;
   *complete = false;
   gzFile f = gzopen(path, "rb");
   if (!f)
      return contents;

   char buf[256];
   int n;
   while ((n = gzread(f, buf, sizeof(buf))) > 0)
      contents.append(buf, n);

   int err;
   gzerror(f, &err);
   *complete = n == 0 && err == Z_OK;
   gzclose(f);
   return contents;
}
#endif

static int
count_lines_starting_with(const std::string &contents, const std::string &prefix)
{
   int count = 0;
   for (size_t pos = 0; pos < contents.size();) {
      if (contents.compare(pos, prefix.size(), prefix) == 0)
         count++;
      size_t newline = contents.find('\n', pos);
      if (newline == std::string::npos)
         break;
      pos = newline + 1;
   }
   return count;
}

static int
test_thread(void *_state)
{
   struct u_trace_context ctx;
   init_context(&ctx);
   u_trace_context_fini(&ctx);

   return 0;
}

TEST_F(UtilPerfTraceTest, Multithread)
{
   thrd_t threads[NUM_DEBUG_TEST_THREAD];
   os_set_option("MESA_GPU_TRACEFILE", trace_file_path, true);
   /* No "%i", so all threads share one file; exercises shared_out_mtx. */
   os_set_option("MESA_GPU_TRACES", "print_csv", true);

   for (unsigned i = 0; i < NUM_DEBUG_TEST_THREAD; i++) {
        thrd_create(&threads[i], test_thread, NULL);
   }
   for (unsigned i = 0; i < NUM_DEBUG_TEST_THREAD; i++) {
      int ret;
      thrd_join(threads[i], &ret);
   }
   u_trace_state_reset();

   /* Each thread writes one known header line and one blank line; check
    * none got torn or merged by concurrent writers.
    */
   std::string out = read_file(trace_file_path);
   EXPECT_EQ(count_lines_starting_with(out, "frame,batch,time_ns,event,"),
             NUM_DEBUG_TEST_THREAD) << out;
   int newlines = 0;
   for (char c : out) {
      if (c == '\n')
         newlines++;
   }
   EXPECT_EQ(newlines, 2 * NUM_DEBUG_TEST_THREAD) << out;
}

TEST_F(UtilPerfTraceTest, EndOfFrameOnce)
{
   os_set_option("MESA_GPU_TRACEFILE", trace_file_path, true);
   os_set_option("MESA_GPU_TRACES", "print", true);

   struct u_trace_context ctx;
   init_context(&ctx);
   process_frame(&ctx);
   u_trace_context_fini(&ctx);
   u_trace_state_reset();

   std::string out = read_file(trace_file_path);
   EXPECT_EQ(count_lines_starting_with(out, "END OF FRAME 0 (ctx 0)"), 1) << out;
}

TEST_F(UtilPerfTraceTest, PerContextFiles)
{
   std::string path_template = trace_file_with("-%i");
   os_set_option("MESA_GPU_TRACEFILE", path_template.c_str(), true);
   os_set_option("MESA_GPU_TRACES", "print", true);

   struct u_trace_context ctx[2];
   for (unsigned i = 0; i < ARRAY_SIZE(ctx); i++) {
      init_context(&ctx[i]);
      process_frame(&ctx[i]);
      u_trace_context_fini(&ctx[i]);
   }
   u_trace_state_reset();

   for (unsigned i = 0; i < ARRAY_SIZE(ctx); i++) {
      std::string path = trace_file_with(("-" + std::to_string(i)).c_str());
      std::string out = read_file(path.c_str());
      std::string marker = "END OF FRAME 0 (ctx " + std::to_string(i) + ")";
      EXPECT_EQ(count_lines_starting_with(out, marker), 1) << out;
      remove(path.c_str());
   }
}

TEST_F(UtilPerfTraceTest, PerProcessFile)
{
   std::string path_template = trace_file_with("-%p");
   os_set_option("MESA_GPU_TRACEFILE", path_template.c_str(), true);
   os_set_option("MESA_GPU_TRACES", "print", true);

   struct u_trace_context ctx;
   init_context(&ctx);
   process_frame(&ctx);
   u_trace_context_fini(&ctx);
   u_trace_state_reset();

   std::string path =
      trace_file_with(("-" + std::to_string(test_getpid())).c_str());
   std::string out = read_file(path.c_str());
   EXPECT_EQ(count_lines_starting_with(out, "END OF FRAME 0 (ctx 0)"), 1) << out;
   remove(path.c_str());
}

#if U_TRACE_HAS_COMPRESS
TEST_F(UtilPerfTraceTest, GzipPerContextFiles)
{
   std::string path_template = trace_file_with("-%i.gz");
   os_set_option("MESA_GPU_TRACEFILE", path_template.c_str(), true);
   os_set_option("MESA_GPU_TRACES", "print", true);

   struct u_trace_context ctx[2];
   for (unsigned i = 0; i < ARRAY_SIZE(ctx); i++) {
      init_context(&ctx[i]);
      process_frame(&ctx[i]);
      u_trace_context_fini(&ctx[i]);
   }
   u_trace_state_reset();

   for (unsigned i = 0; i < ARRAY_SIZE(ctx); i++) {
      std::string path = trace_file_with(("-" + std::to_string(i) + ".gz").c_str());
      bool complete;
      std::string out = read_gz_file(path.c_str(), &complete);
      EXPECT_TRUE(complete) << path;
      std::string marker = "END OF FRAME 0 (ctx " + std::to_string(i) + ")";
      EXPECT_EQ(count_lines_starting_with(out, marker), 1) << out;
      remove(path.c_str());
   }
}

TEST_F(UtilPerfTraceTest, GzipSharedFile)
{
   std::string path = trace_file_with(".gz");
   os_set_option("MESA_GPU_TRACEFILE", path.c_str(), true);
   os_set_option("MESA_GPU_TRACES", "print", true);

   struct u_trace_context ctx[2];
   for (unsigned i = 0; i < ARRAY_SIZE(ctx); i++) {
      init_context(&ctx[i]);
      process_frame(&ctx[i]);
      u_trace_context_fini(&ctx[i]);
   }
   /* The shared stream is only closed here, with the process-wide state. */
   u_trace_state_reset();

   bool complete;
   std::string out = read_gz_file(path.c_str(), &complete);
   EXPECT_TRUE(complete);
   for (unsigned i = 0; i < ARRAY_SIZE(ctx); i++) {
      std::string marker = "END OF FRAME 0 (ctx " + std::to_string(i) + ")";
      EXPECT_EQ(count_lines_starting_with(out, marker), 1) << out;
   }
   remove(path.c_str());
}
#endif
