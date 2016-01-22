/* AUTOGENERATED FILE. DO NOT EDIT. */

//=======Test Runner Used To Run Each Test Below=====
#define RUN_TEST(TestFunc, TestLineNum) \
{ \
  Unity.CurrentTestName = #TestFunc; \
  Unity.CurrentTestLineNumber = TestLineNum; \
  Unity.NumberOfTests++; \
  if (TEST_PROTECT()) \
  { \
      setUp(); \
      TestFunc(); \
  } \
  if (TEST_PROTECT() && !TEST_IS_IGNORED) \
  { \
    tearDown(); \
  } \
  UnityConcludeTest(); \
}

//=======Automagically Detected Files To Include=====
#include "unity.h"
#include <setjmp.h>
#include <stdio.h>
#include "config.h"
#include "ntp.h"
#include "ntp_stdlib.h"

//=======External Functions This Runner Calls=====
extern void setUp(void);
extern void tearDown(void);
extern void test_Encrypt(void);
extern void test_DecryptValid(void);
extern void test_DecryptInvalid(void);
extern void test_IPv4AddressToRefId(void);
extern void test_IPv6AddressToRefId(void);


//=======Test Reset Option=====
void resetTest(void);
void resetTest(void)
{
  tearDown();
  setUp();
}

char const *progname;


//=======MAIN=====
int main(int argc, char *argv[])
{
  progname = argv[0];
  UnityBegin("a_md5encrypt.c");
  RUN_TEST(test_Encrypt, 40);
  RUN_TEST(test_DecryptValid, 41);
  RUN_TEST(test_DecryptInvalid, 42);
  RUN_TEST(test_IPv4AddressToRefId, 43);
  RUN_TEST(test_IPv6AddressToRefId, 44);

  return (UnityEnd());
}
