//------------------------------------------------------------------------------
// Copyright (c) 2011 by European Organization for Nuclear Research (CERN)
// Author: Lukasz Janyst <ljanyst@cern.ch>
// See the LICENCE file for details.
//------------------------------------------------------------------------------

#include <cppunit/extensions/HelperMacros.h>
#include "XrdCl/XrdClURL.hh"
#include "XrdCl/XrdClAnyObject.hh"
#include "XrdCl/XrdClTaskManager.hh"

//------------------------------------------------------------------------------
// Declaration
//------------------------------------------------------------------------------
class UtilsTest: public CppUnit::TestCase
{
  public:
    CPPUNIT_TEST_SUITE( UtilsTest );
      CPPUNIT_TEST( urlTest );
      CPPUNIT_TEST( anyTest );
      CPPUNIT_TEST( taskManagerTest );
    CPPUNIT_TEST_SUITE_END();
    void urlTest();
    void anyTest();
    void taskManagerTest();
};

CPPUNIT_TEST_SUITE_REGISTRATION( UtilsTest );

//------------------------------------------------------------------------------
// URL test
//------------------------------------------------------------------------------
void UtilsTest::urlTest()
{
  XrdClient::URL url1( "root://user1:passwd1@host1:123//path?param1=val1&param2=val2" );
  XrdClient::URL url2( "root://user1@host1//path?param1=val1&param2=val2" );
  XrdClient::URL url3( "root://host1" );
  XrdClient::URL urlInvalid1( "root://user1:passwd1@host1:asd//path?param1=val1&param2=val2" );
  XrdClient::URL urlInvalid2( "root://user1:passwd1host1:123//path?param1=val1&param2=val2" );
  XrdClient::URL urlInvalid3( "root:////path?param1=val1&param2=val2" );
  XrdClient::URL urlInvalid4( "root://@//path?param1=val1&param2=val2" );
  XrdClient::URL urlInvalid5( "root://:@//path?param1=val1&param2=val2" );
  XrdClient::URL urlInvalid6( "root://" );
  XrdClient::URL urlInvalid7( "://asds" );
  XrdClient::URL urlInvalid8( "root://asd@://path?param1=val1&param2=val2" );

  //----------------------------------------------------------------------------
  // Full url
  //----------------------------------------------------------------------------
  CPPUNIT_ASSERT( url1.IsValid() == true );
  CPPUNIT_ASSERT( url1.GetProtocol() == "root" );
  CPPUNIT_ASSERT( url1.GetUserName() == "user1" );
  CPPUNIT_ASSERT( url1.GetPassword() == "passwd1" );
  CPPUNIT_ASSERT( url1.GetHostName() == "host1" );
  CPPUNIT_ASSERT( url1.GetPort() == 123 );
  CPPUNIT_ASSERT( url1.GetPathWithParams() == "/path?param1=val1&param2=val2" );
  CPPUNIT_ASSERT( url1.GetPath() == "/path" );
  CPPUNIT_ASSERT( url1.GetParams().size() == 2 );

  XrdClient::URL::ParamsMap::const_iterator it;
  it = url1.GetParams().find( "param1" );
  CPPUNIT_ASSERT( it != url1.GetParams().end() );
  CPPUNIT_ASSERT( it->second == "val1" );
  it = url1.GetParams().find( "param2" );
  CPPUNIT_ASSERT( it != url1.GetParams().end() );
  CPPUNIT_ASSERT( it->second == "val2" );
  it = url1.GetParams().find( "param3" );
  CPPUNIT_ASSERT( it == url1.GetParams().end() );

  //----------------------------------------------------------------------------
  // No password, no port
  //----------------------------------------------------------------------------
  CPPUNIT_ASSERT( url2.IsValid() == true );
  CPPUNIT_ASSERT( url2.GetProtocol() == "root" );
  CPPUNIT_ASSERT( url2.GetUserName() == "user1" );
  CPPUNIT_ASSERT( url2.GetPassword() == "" );
  CPPUNIT_ASSERT( url2.GetHostName() == "host1" );
  CPPUNIT_ASSERT( url2.GetPort() == 1094 );
  CPPUNIT_ASSERT( url2.GetPath() == "/path" );
  CPPUNIT_ASSERT( url2.GetPathWithParams() == "/path?param1=val1&param2=val2" );
  CPPUNIT_ASSERT( url1.GetParams().size() == 2 );

  it = url2.GetParams().find( "param1" );
  CPPUNIT_ASSERT( it != url2.GetParams().end() );
  CPPUNIT_ASSERT( it->second == "val1" );
  it = url2.GetParams().find( "param2" );
  CPPUNIT_ASSERT( it != url2.GetParams().end() );
  CPPUNIT_ASSERT( it->second == "val2" );
  it = url2.GetParams().find( "param3" );
  CPPUNIT_ASSERT( it == url2.GetParams().end() );

  //----------------------------------------------------------------------------
  // Just the host and the protocol
  //----------------------------------------------------------------------------
  CPPUNIT_ASSERT( url3.IsValid() == true );
  CPPUNIT_ASSERT( url3.GetProtocol() == "root" );
  CPPUNIT_ASSERT( url3.GetUserName() == "" );
  CPPUNIT_ASSERT( url3.GetPassword() == "" );
  CPPUNIT_ASSERT( url3.GetHostName() == "host1" );
  CPPUNIT_ASSERT( url3.GetPort() == 1094 );
  CPPUNIT_ASSERT( url3.GetPath() == "" );
  CPPUNIT_ASSERT( url3.GetPathWithParams() == "" );
  CPPUNIT_ASSERT( url3.GetParams().size() == 0 );

  //----------------------------------------------------------------------------
  // Bunch od invalid ones
  //----------------------------------------------------------------------------
  CPPUNIT_ASSERT( urlInvalid1.IsValid() == false );
  CPPUNIT_ASSERT( urlInvalid2.IsValid() == false );
  CPPUNIT_ASSERT( urlInvalid3.IsValid() == false );
  CPPUNIT_ASSERT( urlInvalid4.IsValid() == false );
  CPPUNIT_ASSERT( urlInvalid5.IsValid() == false );
  CPPUNIT_ASSERT( urlInvalid6.IsValid() == false );
  CPPUNIT_ASSERT( urlInvalid7.IsValid() == false );
  CPPUNIT_ASSERT( urlInvalid8.IsValid() == false );
}

class A
{
  public:
    double a;
};

class B
{
  public:
    int b;
};

//------------------------------------------------------------------------------
// Any test
//------------------------------------------------------------------------------
void UtilsTest::anyTest()
{
  A *a = new A;
  A *a1 = 0;
  B *b  = 0;
  XrdClient::AnyObject any;

  any.Set( a );
  any.Get( b );
  any.Get( a1 );

  CPPUNIT_ASSERT( !b );
  CPPUNIT_ASSERT( a );
}

//------------------------------------------------------------------------------
// Some tasks that do something
//------------------------------------------------------------------------------
class TestTask1: public XrdClient::Task
{
  public:
    TestTask1( std::vector<time_t> &runs ): pRuns( runs ) {}

    virtual time_t Run( time_t now )
    {
      pRuns.push_back( now );
      return 0;
    }

  private:
    std::vector<time_t> &pRuns;
};

class TestTask2: public XrdClient::Task
{
  public:
    TestTask2( std::vector<time_t> &runs ): pRuns( runs ) {}

    virtual time_t Run( time_t now )
    {
      pRuns.push_back( now );
      if( pRuns.size() >= 5 )
        return 0;
      return now+2;
    }

  private:
    std::vector<time_t> &pRuns;
};

//------------------------------------------------------------------------------
// Task Manager test
//------------------------------------------------------------------------------
void UtilsTest::taskManagerTest()
{
  using namespace XrdClient;

  std::vector<time_t> runs1, runs2;
  Task *tsk1 = new TestTask1( runs1 );
  Task *tsk2 = new TestTask2( runs2 );

  TaskManager taskMan;
  CPPUNIT_ASSERT( taskMan.Start() );

  time_t now = ::time(0);
  taskMan.RegisterTask( tsk1, now+2 );
  taskMan.RegisterTask( tsk2, now+1 );

  ::sleep( 6 );
  taskMan.UnregisterTask( tsk1 );
  taskMan.UnregisterTask( tsk2 );

  ::sleep( 2 );

  CPPUNIT_ASSERT( runs1.size() == 1 );
  CPPUNIT_ASSERT( runs2.size() == 3 );
  CPPUNIT_ASSERT( taskMan.Stop() );
}
