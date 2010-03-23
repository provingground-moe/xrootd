/******************************************************************************/
/*                                                                            */
/*                       X r d F r m C o n f i g . c c                        */
/*                                                                            */
/* (c) 2009 by the Board of Trustees of the Leland Stanford, Jr., University  */
/*       All Rights Reserved. See XrdInfo.cc for complete License Terms       */
/*   Produced by Andrew Hanushevsky for Stanford University under contract    */
/*                DE-AC02-76-SFO0515 with the Deprtment of Energy             */
/******************************************************************************/

//         $Id$

const char *XrdFrmConfigCVSID = "$Id$";
  
#include <unistd.h>
#include <ctype.h>
#include <dirent.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "Xrd/XrdInfo.hh"
#include "XrdCms/XrdCmsNotify.hh"
#include "XrdFrm/XrdFrmConfig.hh"
#include "XrdFrm/XrdFrmTrace.hh"  // Add to GNUmake
#include "XrdFrm/XrdFrmUtils.hh"
#include "XrdNet/XrdNetDNS.hh"
#include "XrdOss/XrdOss.hh"
#include "XrdOss/XrdOssSpace.hh"
#include "XrdOuc/XrdOuca2x.hh"
#include "XrdOuc/XrdOucEnv.hh"
#include "XrdOuc/XrdOucExport.hh"
#include "XrdOuc/XrdOucMsubs.hh"
#include "XrdOuc/XrdOucName2Name.hh"
#include "XrdOuc/XrdOucProg.hh"
#include "XrdOuc/XrdOucStream.hh"
#include "XrdOuc/XrdOucPList.hh"
#include "XrdOuc/XrdOucTList.hh"
#include "XrdOuc/XrdOucTokenizer.hh" // Add to GNUmake
#include "XrdOuc/XrdOucUtils.hh"
#include "XrdSys/XrdSysError.hh"
#include "XrdSys/XrdSysHeaders.hh"
#include "XrdSys/XrdSysLogger.hh"
#include "XrdSys/XrdSysPlugin.hh"
#include "XrdSys/XrdSysTimer.hh"
#include "XrdSys/XrdSysPlatform.hh"
#include "XrdSys/XrdSysPthread.hh"
#include "XrdXrootd/XrdXrootdMonitor.hh"

using namespace XrdFrm;

/******************************************************************************/
/*                         L o c a l   C l a s s e s                          */
/******************************************************************************/
  
class XrdFrmConfigSE
{
public:

XrdSysSemaphore mySem;
int             myFD;
int             seFD;
int             BLen;
char            Buff[32000];

                XrdFrmConfigSE() : mySem(0), myFD(-1), seFD(-1), BLen(0) {}
               ~XrdFrmConfigSE() {}
};

/******************************************************************************/
/*                     T h r e a d   I n t e r f a c e s                      */
/******************************************************************************/

void *XrdFrmConfigMum(void *parg)
{
   XrdFrmConfigSE *theSE = (XrdFrmConfigSE *)parg;
   char *bp = theSE->Buff;
   int  n, bleft = sizeof(theSE->Buff)-2;

// Let the calling thread continue at this point
//
   theSE->mySem.Post();

// Read everything we can
//
   do {if ((n = read(theSE->myFD, bp, bleft)) <= 0)
          {if (!n || (n < 0 && errno != EINTR)) break;}
       bp += n;
      } while ((bleft -= n));

// Refalgomize everything
//
   dup2(theSE->seFD, STDERR_FILENO);
   close(theSE->seFD);

// Check if we should add a newline character
//
   if (theSE->Buff[bp-(theSE->Buff)-1] != '\n') *bp++ = '\n';
   theSE->BLen = bp-(theSE->Buff);

// All done
//
   theSE->mySem.Post();
// pthread_exit((void *)0);
   return (void *)0;
}

void *XrdLogWorker(void *parg)
{
   time_t midnite = XrdSysTimer::Midnight() + 86400;
   char *mememe = strdup((char *)parg);

   while(1)
        {XrdSysTimer::Snooze(midnite-time(0));
         Say.Say(0, XrdBANNER);
         Say.Say(0, mememe, " running.");
         midnite += 86400;
        }
   return (void *)0;
}

/******************************************************************************/
/*                           C o n s t r u c t o r                            */
/******************************************************************************/
  
XrdFrmConfig::XrdFrmConfig(SubSys ss, const char *vopts, const char *uinfo)
             : dfltPolicy("*", -2, -3, 72000, 0)
{
   char *sP, buff[128];

// Preset all variables with common defaults
//
   vOpts    = vopts;
   uInfo    = uinfo;
   ssID     = ss;
   AdminPath= 0;
   AdminMode= 0740;
   xfrMax   = 1;
   WaitTime = 300;
   MSSCmd   = 0;
   xfrCmd   = strdup("/opt/xrootd/utils/frm_xfr -p $OFLAG $RFN $PFN");
   xfrVec   = 0;
   qPath    = 0;
   isAgent  = (getenv("XRDADMINPATH") ? 1 : 0);
   ossLib   = 0;
   cmsPath  = 0;
   monStage = 0;
   sSpec    = 0;
   isOTO    = 0;
   Test     = 0;
   Verbose  = 0;
   pathList = 0;
   spacList = 0;
   lockFN   = "DIR_LOCK";  // May be ".DIR_LOCK" if hidden
   cmdHold  = -1;
   cmdFree  = 0;
   pVecNum  = 0;
   pProg    = 0;
   Fix      = 0;
   dirHold  = 40*60*60;

   myUid    = geteuid();
   myGid    = getegid();

   LocalRoot= RemoteRoot = 0;
   lcl_N2N  = rmt_N2N = the_N2N = 0;
   N2N_Lib  = N2N_Parms         = 0;

// Establish our instance name
//
   if ((sP = getenv("XRDNAME")) && *sP) myInsName = sP;
      else myInsName = 0;

// Establish default config file
//
   if (!(sP = getenv("XRDCONFIGFN")) || !*sP) 
            ConfigFN = 0;
      else {ConfigFN = strdup(sP); isAgent = 1;}

// Establish directive postfix
//
        if (ss == ssAdmin) {myFrmid = "admin"; myFrmID = "ADMIN";}
   else if (ss == ssMigr)  {myFrmid = "migr";  myFrmID = "MIGR";}
   else if (ss == ssPstg)  {myFrmid = "pstg";  myFrmID = "PSTG";}
   else if (ss == ssPurg)  {myFrmid = "purge"; myFrmID = "PURG";}
   else                    {myFrmid = "frm";   myFrmID = "FRM";}

// Set correct error prefix
//
   strcpy(buff, myFrmid);
   strcat(buff, "_");
   Say.SetPrefix(strdup(buff));

// Set correct oss type
//
   XrdOucEnv::Export("XRDOSSTYPE", myFrmid);

// Set correct option prefix
//
   strcpy(buff, "frm.");
   strcat(buff, myFrmid);
   strcat(buff, ".");
   pfxDTS = strdup(buff); plnDTS = strlen(buff);
}
  
/******************************************************************************/
/* Public:                     C o n f i g u r e                              */
/******************************************************************************/
  
int XrdFrmConfig::Configure(int argc, char **argv, int (*ppf)())
{
   extern XrdOss *XrdOssGetSS(XrdSysLogger *, const char *, const char *);
   XrdFrmConfigSE theSE;
   int n, retc, isMum = 0, myXfrMax = -1, NoGo = 0;
   const char *temp;
   char c, buff[1024], *logfn = 0;
   long long logkeep = 0;
   extern char *optarg;
   extern int opterr, optopt;

// Obtain the program name (used for logging)
//
    retc = strlen(argv[0]);
    while(retc--) if (argv[0][retc] == '/') break;
    myProg = &argv[0][retc+1];
    vectArg = argv; numcArg = argc;

// Process the options
//
   opterr = 0; nextArg = 1;
   if (argc > 1 && '-' == *argv[1]) 
      while ((c = getopt(argc,argv,vOpts)) && ((unsigned char)c != 0xff))
     { switch(c)
       {
       case 'c': if (ConfigFN) free(ConfigFN);
                 ConfigFN = strdup(optarg);
                 break;
       case 'd': Trace.What |= TRACE_ALL;
                 XrdOucEnv::Export("XRDDEBUG","1");
                 break;
       case 'f': Fix = 1;
                 break;
       case 'h': Usage(0);
       case 'k': n = strlen(optarg)-1;
                 retc = (isalpha(optarg[n])
                        ? XrdOuca2x::a2sz(Say,"keep size", optarg,&logkeep)
                        : XrdOuca2x::a2ll(Say,"keep count",optarg,&logkeep));
                 if (retc) Usage(1);
                 if (!isalpha(optarg[n])) logkeep = -logkeep;
                 break;
       case 'l': if (logfn) free(logfn);
                 logfn = strdup(optarg);
                 break;
       case 'm': if (XrdOuca2x::a2i(Say,"max number",optarg,&myXfrMax))
                    Usage(1);
                 break;
       case 'n': myInsName = optarg;
                 break;
       case 'O': isOTO = 1;
                 if (!ConfigOTO(optarg)) Usage(1);
                 break;
       case 's': sSpec = 1;
                 break;
       case 'T': Test  = 1;
                 break;
       case 'v': Verbose = 1;
                 break;
       case 'w': if (XrdOuca2x::a2tm(Say,"wait time",optarg,&WaitTime))
                    Usage(1);
                 break;
       default:  sprintf(buff,"'%c'", optopt);
                 if (c == ':') Say.Emsg("Config", buff, "value not specified.");
                    else Say.Emsg("Config", buff, "option is invalid");
                 Usage(1);
       }
     nextArg = optind;
     }

// If we are an agent without a logfile and one is actually defined for the
// underlying system, use the directory of the underlying system.
//
   if (ssID != ssAdmin)
      {if (!logfn)
          {if (isAgent && (logfn = getenv("XRDLOGDIR")))
              {sprintf(buff, "%s%s%clog", logfn, myFrmid, (isAgent ? 'a' : 'd'));
               logfn = strdup(buff);
              }
          } else if (!(logfn=XrdOucUtils::subLogfn(Say,myInsName,logfn))) _exit(16);

   // Bind the log file if we have one
   //
       if (logfn)
          {if (logkeep) Logger.setKeep(logkeep);
           Logger.Bind(logfn, 24*60*60);
          }
       }

// Get the full host name. In theory, we should always get some kind of name.
//
   if (!(myName = XrdNetDNS::getHostName()))
      {Say.Emsg("Config","Unable to determine host name; execution terminated.");
       _exit(16);
      }

// Set the Environmental variables to hold some config information
// XRDINSTANCE=<pgm> <instance name>@<host name>
//
   sprintf(buff,"XRDINSTANCE=%s %s@%s",myProg,(myInst ? myInst:"anon"),myName);
   putenv(strdup(buff)); // XRDINSTANCE
   myInstance = strdup(index(buff,'=')+1);
   XrdOucEnv::Export("XRDHOST", myName);
   XrdOucEnv::Export("XRDPROG", myProg);
   if (myInsName) XrdOucEnv::Export("XRDNAME", myInsName);

// We need to divert the output if we are in admin mode with no logfile
//
   if (!logfn && (ssID == ssAdmin || isOTO) && !Trace.What)
      isMum = ConfigMum(theSE);

// Put out the herald
//
   sprintf(buff, "Scalla %s is starting. . .", myProg);
   Say.Say(0, buff);
   Say.Say(XrdBANNER);

// Process the configuration file.
//
   Say.Say("++++++ ", myInstance, " initialization started.");
   if (!ConfigFN || !*ConfigFN) ConfigFN = strdup("/opt/xrootd/etc/xrootd.cf");
   Say.Say("Config using configuration file ", ConfigFN);
   NoGo = ConfigProc();

// Create the correct admin path
//
   if (!NoGo) NoGo = ConfigPaths();

// Obtain and configure the oss (lightweight option only)
//
   if (!isAgent)
      {XrdOucEnv::Export("XRDREDIRECT", "Q");
       XrdOucEnv::Export("XRDOSSTYPE",  myFrmID);
       if (ssID == ssMigr || ssID == ssPurg)
          XrdOucEnv::Export("XRDOSSCSCAN", "off");
       if (!NoGo && !(ossFS=XrdOssGetSS(Say.logger(),ConfigFN,ossLib))) NoGo=1;
      }

// Configure each specific component
//
   if (!NoGo) switch(ssID)
      {case ssAdmin: NoGo = (ConfigN2N() || ConfigMss());
                     break;
       case ssMigr:  NoGo = (ConfigN2N() || ConfigMP("migratable"));
                     break;
       case ssPstg:  if (isAgent) break;
                     if (ConfigN2N() || ConfigMss()
                     ||  !(xfrVec = ConfigCmd("xfrcmd", xfrCmd))
                     ||  (monStage &&!XrdXrootdMonitor::Init(0, &Say)))
                         NoGo = 1;
                     break;
       case ssPurg:  NoGo = (ConfigN2N() || ConfigMP("purgeable"));
                     break;
       default:      break;
      }

// If we have a post-processing routine, invoke it
//
   if (!NoGo && ppf) NoGo = ppf();

// Start the log turn-over thread
//
   if (!NoGo && logfn)
      {pthread_t tid;
       if ((retc = XrdSysThread::Run(&tid, XrdLogWorker, (void *)myInstance,
                                     XRDSYSTHREAD_BIND, "midnight runner")))
          {Say.Emsg("Config", retc, "create logger thread"); NoGo = 1;}
      }

// Print ending message
//
   temp = (NoGo ? " initialization failed." : " initialization completed.");
   Say.Say("------ ", myInstance, temp);

// Finish up mum processing
//
   if (isMum)
      {close(STDERR_FILENO);
       theSE.mySem.Wait();
       if (NoGo && write(STDERR_FILENO, theSE.Buff, theSE.BLen)) {}
      }

// All done
//
   return !NoGo;
}
  
/******************************************************************************/
/* Public:                     L o c a l P a t h                              */
/******************************************************************************/
  
int XrdFrmConfig::LocalPath(const char *oldp, char *newp, int newpsz)
{
    int rc = 0;

    if (lcl_N2N) rc = lcl_N2N->lfn2pfn(oldp, newp, newpsz);
       else if (((int)strlen(oldp)) >= newpsz) rc = ENAMETOOLONG;
               else strcpy(newp, oldp);
    if (rc) {Say.Emsg("Config", rc, "generate local path from", oldp);
             return 0;
            }
    return 1;
}

/******************************************************************************/
/* Public:                    R e m o t e P a t h                             */
/******************************************************************************/
  
int XrdFrmConfig::RemotePath(const char *oldp, char *newp, int newpsz)
{
    int rc = 0;

    if (rmt_N2N) rc = rmt_N2N->lfn2rfn(oldp, newp, newpsz);
       else if (((int)strlen(oldp)) >= newpsz) rc = ENAMETOOLONG;
               else strcpy(newp, oldp);
    if (rc) {Say.Emsg("Config", rc, "generate rmote path from", oldp);
             return 0;
            }
    return 1;
}
  
/******************************************************************************/
/*                                 S p a c e                                  */
/******************************************************************************/
  
XrdOucTList *XrdFrmConfig::Space(const char *Name, const char *Path)
{
   static XrdOucTList nullEnt;
   struct VPInfo *vP = VPList;
          XrdOucTList *tP;
   char buff[1032];
   int n;

// First find the space entry
//
   while(vP && strcmp(vP->Name, Name)) vP = vP->Next;
   if (!vP) return 0;

// Check if we should find a particular path
//
   if (!Path) return vP->Dir;

// Make sure it nds with a slash (it usually does not)
//
   n = strlen(Path)-1;
   if (Path[n] != '/')
      {if (n >= (int)sizeof(buff)-2) return &nullEnt;
       strcpy(buff, Path); buff[n+1] = '/'; buff[n+2] = '\0';
       Path = buff;
      }

// Find the path
//
   tP = vP->Dir;
   while(tP && strcmp(Path, tP->text)) tP = tP->next;
   return (tP ? tP : &nullEnt);
}

/******************************************************************************/
/*                     P r i v a t e   F u n c t i o n s                      */
/******************************************************************************/
/******************************************************************************/
/* Private:                    C o n f i g C m d                              */
/******************************************************************************/
  
XrdOucMsubs *XrdFrmConfig::ConfigCmd(const char *cname, char *cdata)
{
   XrdOucMsubs *msubs;
   char *cP;

   if (!cdata) {Say.Emsg("Config", cname, "not specified."); return 0;}

   if ((cP = index(cdata, ' '))) *cP = '\0';

   if (access(cdata, X_OK))
      {Say.Emsg("Config", errno, "set up", cdata);
       return 0;
      }
   *cP = ' ';

   msubs = new XrdOucMsubs(&Say);
   if (msubs->Parse(cname, cdata)) return msubs;

   return 0;  // We will exit no need to delete msubs
}

/******************************************************************************/
/* Private:                     C o n f i g M P                               */
/******************************************************************************/

int XrdFrmConfig::ConfigMP(const char *pType)
{
   EPNAME("ConfigMP");
   extern XrdOucPListAnchor *XrdOssRPList;
   XrdOucTList *nP, *tP, *mypList = 0, *expList = 0;
   char pDir[MAXPATHLEN+1];
   long long pOpts, xOpt = (*pType == 'm' ? XRDEXP_MIG : XRDEXP_PURGE);
   int i, NoGo = 0;

// Verify that we have an RPList
//
   if (!XrdOssRPList)
      {Say.Emsg("Config", "Cannot determine", pType, "paths."); return 1;}

// Parse the arguments which consist of space names and paths
//
   for (i = nextArg; i < numcArg; i++)
       {char *psVal = vectArg[i];
        int   psLen = strlen(psVal);
        if (*psVal == '/')
           {pOpts = XrdOssRPList->Find(psVal);
            if (pOpts & xOpt) mypList = InsertPL(mypList, psVal, psLen,
                                                (pOpts & XRDEXP_NOTRW ? 0 : 1));
               else {Say.Say("Config", psVal, "not marked", pType); NoGo = 1;}
           } else {
            VPInfo *vP = VPList;
            while(vP && strcmp(psVal, vP->Name)) vP = vP->Next;
            if (vP) spacList = new XrdOucTList(psVal, psLen, spacList);
               else {Say.Emsg("Config", "Space", psVal, "not defined.");
                     NoGo = 1;
                    }
           }
       }

// Check if we should continue
//
   if (NoGo) return 1;

// Get correct path list
//
   if (!mypList)
      {XrdOucPList *fP = XrdOssRPList->First();
       short sval[2];
       while(fP)
            {sval[0] = (fP->Flag() & XRDEXP_NOTRW ? 0 : 1);
             sval[1] = fP->Plen();
             if (fP->Flag() & xOpt)
                 mypList = new XrdOucTList(fP->Path(), sval, mypList);
                 else
                 expList = new XrdOucTList(fP->Path(), sval, expList);
             fP = fP->Next();
            }
       if (!mypList)
          {Say.Emsg("Config", "No", pType, "paths found."); return 1;}
      }

// Now we need to construct a search list which may include excludes which
// hapen when we get nested subtrees with different options
//
   while((tP = mypList))
        {if (!LocalPath(tP->text, pDir, sizeof(pDir))) NoGo = 1;
            else {pathList = new VPInfo(pDir, int(tP->sval[0]), pathList);
                  DEBUG("Will scan " <<(tP->sval[0]?"r/w: ":"r/o: ") <<pDir);
                  nP = expList;
                  while(nP)
                       {if (!strncmp(tP->text, nP->text, tP->sval[1]))
                           InsertXD(nP->text);
                        nP = nP->next;
                       }
                  mypList = tP->next; delete tP;
                 }
        }

// Delete the explist
//
   while((tP = expList)) {expList = tP->next; delete tP;}

// All done now
//
   return NoGo;
}

/******************************************************************************/
/* Private:                    C o n f i g M s s                              */
/******************************************************************************/
  
int XrdFrmConfig::ConfigMss()
{
   if (MSSCmd)
      {MSSProg = new XrdOucProg(&Say);
       if (MSSProg->Setup(MSSCmd)) return 1;
      }
   return 0;
}

/******************************************************************************/
/* Private:                    C o n f i g M u m                              */
/******************************************************************************/

int XrdFrmConfig::ConfigMum(XrdFrmConfigSE &theSE)
{
   class Recover
        {public:
         int fdvec[2];
         int stdErr;
             Recover() : stdErr(-1) {fdvec[0] = -1; fdvec[1] = -1;}
            ~Recover() {if (fdvec[0] >= 0) close(fdvec[0]);
                        if (fdvec[1] >= 0) close(fdvec[1]);
                        if (stdErr >= 0)   {dup2(stdErr, STDERR_FILENO);
                                            close(stdErr);
                                           }
                       }
        };
   Recover FD;
   pthread_t tid;
   int rc;

// Create a pipe
//
   if (pipe(FD.fdvec) < 0) return 0;
   fcntl(FD.fdvec[0], F_SETFD, FD_CLOEXEC);

// Save the current standard error FD
//
   if ((FD.stdErr = dup(STDERR_FILENO)) < 0) return 0;

// Now hook-up the pipe to standard error
//
   if (dup2(FD.fdvec[1], STDERR_FILENO) < 0) return 0;
   close(FD.fdvec[1]); FD.fdvec[1] = -1;

// Prepare arguments to the thread that will suck up the output
//
   theSE.myFD = FD.fdvec[0];
   theSE.seFD = FD.stdErr;

// Start a thread to read all of the output
//
    if ((rc = XrdSysThread::Run(&tid, XrdFrmConfigMum, (void *)&theSE,
                                XRDSYSTHREAD_BIND, "Mumify"))) return 0;

// Now fixup to return correctly
//
   theSE.mySem.Wait();
   FD.fdvec[0] = -1;
   FD.stdErr = -1;
   return 1;
}

/******************************************************************************/
/* Private:                    C o n f i g N 2 N                              */
/******************************************************************************/

int XrdFrmConfig::ConfigN2N()
{
   XrdSysPlugin    *myLib;
   XrdOucName2Name *(*ep)(XrdOucgetName2NameArgs);

// If we have no library path then use the default method (this will always
// succeed).
//
   if (!N2N_Lib)
      {the_N2N = XrdOucgetName2Name(&Say, ConfigFN, "", LocalRoot, RemoteRoot);
       if (LocalRoot)  lcl_N2N = the_N2N;
       if (RemoteRoot) rmt_N2N = the_N2N;
       return 0;
      }

// Create a pluin object (we will throw this away without deletion because
// the library must stay open but we never want to reference it again).
//
   if (!(myLib = new XrdSysPlugin(&Say, N2N_Lib))) return 1;

// Now get the entry point of the object creator
//
   ep = (XrdOucName2Name *(*)(XrdOucgetName2NameArgs))(myLib->getPlugin("XrdOucgetName2Name"));
   if (!ep) return 1;


// Get the Object now
//
   lcl_N2N = rmt_N2N = the_N2N = ep(&Say, ConfigFN, 
                                   (N2N_Parms ? N2N_Parms : ""),
                                   LocalRoot, RemoteRoot);
   return lcl_N2N == 0;
}
  
/******************************************************************************/
/*                             C o n f i g O T O                              */
/******************************************************************************/
  
int XrdFrmConfig::ConfigOTO(char *Parms)
{
   char *Comma;

// Pick up free argument
//
   if ((Comma = index(Parms, ','))) *Comma = '\0';
   if (XrdOuca2x::a2sp(Say, "free value", Parms, &cmdFree, 1)) return 0;

// Pick up hold argument
//
   if (!Comma || !(*(Comma+1))) return 1;
   if (*(Comma+1) == ',') Comma++;
      else {Parms = Comma+1;
            if ((Comma = index(Parms, ','))) *Comma = '\0';
            if (XrdOuca2x::a2i(Say,"hold value",Parms,&cmdHold,0)) return 0;
           }

// All done
//
   return 1;
}

/******************************************************************************/
/*                           C o n f i g P a t h s                            */
/******************************************************************************/
  
int XrdFrmConfig::ConfigPaths()
{
   char *xPath, *yPath, buff[MAXPATHLEN]; 
   const char *insName;
   int retc;

// Establish the cmsd notification path
//
   

// Set the directory where the meta information is to go
//  XRDADMINPATH already contains the instance name

   if ( (!AdminPath) && (xPath = getenv("XRDADMINPATH"))) {
       insName = 0;
   }
   else {
       if (!(xPath = AdminPath))
           xPath = (char *)"/tmp/";
       insName = myInsName;
   }
   
   cmsPath = new XrdCmsNotify(&Say, xPath, insName, XrdCmsNotify::isServ);

   yPath = XrdOucUtils::genPath(xPath, insName, "frm");
   if (AdminPath) free(AdminPath); AdminPath = yPath;

// Create the admin directory if it does not exists
//
   if ((retc = XrdOucUtils::makePath(AdminPath, AdminMode)))
      {Say.Emsg("Config", retc, "create admin directory", AdminPath);
       return 0;
      }

// Now we should create a home directory for core files
//
   if (myInsName) XrdOucUtils::makeHome(Say, myInsName);

// Set up the stop file path
//
   if (!StopFile)
      {sprintf(buff,"%sSTOP%s", AdminPath, myFrmID); StopFile = strdup(buff);}

// If a qpath was specified, differentiate it by the instance name
//
   if (qPath)
      {xPath = XrdOucUtils::genPath(qPath, myInsName, "frm");
       free(qPath); qPath = xPath;
      }

// All done
//
   return 0;
}

/******************************************************************************/
/* Private:                   C o n f i g P r o c                             */
/******************************************************************************/
  
int XrdFrmConfig::ConfigProc()
{
  char *var;
  int  cfgFD, retc, mbok, NoGo = 0;
  XrdOucEnv myEnv;
  XrdOucStream cfgFile(&Say, myInstance, &myEnv, "=====> ");

// Try to open the configuration file.
//
   if ( (cfgFD = open(ConfigFN, O_RDONLY, 0)) < 0)
      {Say.Emsg("Config", errno, "open config file", ConfigFN);
       return 1;
      }
   cfgFile.Attach(cfgFD); cFile = &cfgFile;

// Now start reading records until eof.
//
   while((var = cFile->GetMyFirstWord()))
        {mbok = 0;
         if (!strncmp(var, pfxDTS, plnDTS)) {var += plnDTS; mbok = 1;}
         if(ConfigXeq(var, mbok)) {cfgFile.Echo(); NoGo = 1;}
        }

// Now check if any errors occured during file i/o
//
   if ((retc = cfgFile.LastError()))
      NoGo = Say.Emsg("Config", retc, "read config file", ConfigFN);
   cfgFile.Close(); cFile = 0;

// Return final return code
//
   return NoGo;
}

/******************************************************************************/
/* Prvate:                     C o n f i g X e q                              */
/******************************************************************************/

int XrdFrmConfig::ConfigXeq(char *var, int mbok)
{

// Process common items to all subsystems
//
   if (!strcmp(var, "all.adminpath" )) return xapath();

// Process directives specific to each subsystem
//
   if (ssID == ssAdmin)
      {
       if (!strcmp(var, "ofs.osslib"    )) return Grab(var, &ossLib,    0);
       if (!strcmp(var, "oss.cache"     )) return xcache();
       if (!strcmp(var, "oss.localroot" )) return Grab(var, &LocalRoot, 0);
       if (!strcmp(var, "oss.namelib"   )) return xnml();
       if (!strcmp(var, "oss.remoteroot")) return Grab(var, &RemoteRoot, 0);
//     if (!strcmp(var, "oss.mssgwcmd"  )) return Grab(var, &MSSCmd,    0);
//     if (!strcmp(var, "oss.msscmd"    )) return Grab(var, &MSSCmd,    0);
      }

   if (ssID == ssMigr)
      {
       if (!strcmp(var, "stopfile"      )) return Grab(var, &StopFile,  0);
       if (!strcmp(var, "waittime"      )) return xwtm();
      }

   if (ssID == ssPstg)
      {
       if (!strcmp(var, "ofs.osslib"    )) return Grab(var, &ossLib,    0);
       if (!strcmp(var, "oss.cache"     )) return xcache();
       if (!strcmp(var, "oss.localroot" )) return Grab(var, &LocalRoot, 0);
       if (!strcmp(var, "oss.namelib"   )) return xnml();
       if (!strcmp(var, "oss.remoteroot")) return Grab(var, &RemoteRoot, 0);
       if (!strcmp(var, "stopfile"      )) return Grab(var, &StopFile,  0);
       if (!strcmp(var, "waittime"      )) return xwtm();
       if (!strcmp(var, "xrootd.monitor")) return xmon();
       if (!strcmp(var, "xfrmax"        )) return xmaxx();
       if (!strcmp(var, "xfrcmd"        )) return Grab(var, &xfrCmd,    1);
       if (!strcmp(var, "queuepath"     )) return Grab(var, &qPath,     0);
      }

   if (ssID == ssPurg)
      {
       if (!strcmp(var, "dirhold"       )) return xdpol();
       if (!strcmp(var, "oss.cache"     )) return xcache(1);
       if (!strcmp(var, "ofs.osslib"    )) return Grab(var, &ossLib,    0);
       if (!strcmp(var, "policy"        )) return xpol();
       if (!strcmp(var, "polprog"       )) return xpolprog();
       if (!strcmp(var, "queuepath"     )) return Grab(var, &qPath,     0);
       if (!strcmp(var, "stopfile"      )) return Grab(var, &StopFile,  0);
       if (!strcmp(var, "waittime"      )) return xwtm();
      }

   // No match found, complain.
   //
   if (!mbok) cFile->noEcho();
      else {Say.Say("Config warning: ignoring unknown frm directive '",var,"'.");
            cFile->Echo();
           }
   return 0;
}

/******************************************************************************/
/* Private:                      g e t T i m e                                */
/******************************************************************************/

int XrdFrmConfig::getTime(const char *emsg, const char *item, int *val,
                          int minv, int maxv)
{
    if (strcmp(item, "forever"))
       return  XrdOuca2x::a2tm(Say, emsg, item, val, minv, maxv);
    *val = -1;
    return 0;
}
  
/******************************************************************************/
/* Private:                         G r a b                                   */
/******************************************************************************/
  
int XrdFrmConfig::Grab(const char *var, char **Dest, int nosubs)
{
    char  myVar[80], buff[2048], *val;
    XrdOucEnv *myEnv = 0;

// Copy the variable name as this may change because it points to an
// internal buffer in Config. The vagaries of effeciency.
//
   strlcpy(myVar, var, sizeof(myVar));
   var = myVar;

// If substitutions allowed then we need to grab a single token else grab
// the remainder of the line but suppress substitutions.
//
   if (!nosubs) val = cFile->GetWord();
      else {myEnv = cFile->SetEnv(0);
            if (!cFile->GetRest(buff, sizeof(buff)))
               {Say.Emsg("Config", "arguments too long for", var);
                cFile->SetEnv(myEnv);
                return 1;
               }
            val = buff;
            cFile->SetEnv(myEnv);
           }

// At this point, make sure we have a value
//
   if (!val || !(*val))
      {Say.Emsg("Config", "no value for directive", var);
       return 1;
      }

// Set the value
//
   if (*Dest) free(*Dest);
   *Dest = strdup(val);
   return 0;
}

/******************************************************************************/
/* Private:                     I n s e r t P L                               */
/******************************************************************************/
  
XrdOucTList *XrdFrmConfig::InsertPL(XrdOucTList *pL, const char *Path,
                                    int Plen, int isRW)
{
   short sval[2] = {isRW, Plen};
   XrdOucTList *pP = 0, *tP = pL;

// Find insertion point
//
   while(tP && tP->sval[1] < Plen) {pP = tP; tP = tP->next;}

// Insert new element
//
   if (pP) pP->next = new XrdOucTList(Path, sval, tP);
      else       pL = new XrdOucTList(Path, sval, tP);

// Return the new list
//
   return pL;
}

/******************************************************************************/
/* Private:                     I n s e r t X D                               */
/******************************************************************************/

void XrdFrmConfig::InsertXD(const char *Path)
{
   EPNAME("InsertXD");
   char pBuff[MAXPATHLEN], *pP;
   int n = strlen(Path);

// Make sure this does not end with a slash
//
   strcpy(pBuff, Path);
   pP = pBuff + n - 1;
   while(*pP == '/' && pP != pBuff) {*pP-- = '\0'; n--;}

// Insert this directory into the exclude list for the current path
//
   pathList->Dir = new XrdOucTList(pBuff, n, pathList->Dir);
   DEBUG("Excluding '" <<pBuff <<"'");
}
  
/******************************************************************************/
/* Private:                        U s a g e                                  */
/******************************************************************************/
  
void XrdFrmConfig::Usage(int rc)
{
     cerr <<"\nUsage: " <<myProg <<" " <<uInfo <<endl;
     _exit(rc);
}

/******************************************************************************/
/* Private:                       x a p a t h                                 */
/******************************************************************************/

/* Function: xapath

   Purpose:  To parse the directive: adminpath <path> [group]

             <path>    the path of the FIFO to use for admin requests.

             group     allows group access to the admin path

   Output: 0 upon success or !0 upon failure.
*/

int XrdFrmConfig::xapath()
{
    char *pval, *val;
    mode_t mode = S_IRWXU;

// Get the path
//
   pval = cFile->GetWord();
   if (!pval || !pval[0])
      {Say.Emsg("Config", "adminpath not specified"); return 1;}

// Make sure it's an absolute path
//
   if (*pval != '/')
      {Say.Emsg("Config", "adminpath not absolute"); return 1;}

// Record the path
//
   if (AdminPath) free(AdminPath);
   AdminPath = strdup(pval);

// Get the optional access rights
//
   if ((val = cFile->GetWord()) && val[0])
      {if (!strcmp("group", val)) mode |= S_IRWXG;
          else {Say.Emsg("Config", "invalid admin path modifier -", val);
                return 1;
               }
      }
   AdminMode = mode;
   return 0;
}

/******************************************************************************/
/*                                x c a c h e                                 */
/******************************************************************************/

/* Function: xcache

   Purpose:  To parse the directive: cache <group> <path> [xa]

             <group>  logical group name for the cache filesystem.
             <path>   path to the cache.
             xa       support extended attributes

   Output: 0 upon success or !0 upon failure.
*/

int XrdFrmConfig::xcache(int isPrg)
{
   char *val, *pfxdir, *sfxdir;
   char grp[XrdOssSpace::minSNbsz], fn[MAXPATHLEN], dn[MAXNAMLEN];
   int i, k, rc, pfxln, isxa = 0, cnum = 0;
   struct dirent *dp;
   struct stat buff;
   DIR *DFD;

   if (!(val = cFile->GetWord()))
      {Say.Emsg("Config", "cache group not specified"); return 1;}
   if (strlen(val) >= (int)sizeof(grp))
      {Say.Emsg("Config","excessively long cache name - ",val); return 1;}
   strcpy(grp, val);

   if (!(val = cFile->GetWord()))
      {Say.Emsg("Config", "cache path not specified"); return 1;}

   k = strlen(val);
   if (k >= (int)(sizeof(fn)-1) || val[0] != '/' || k < 2)
      {Say.Emsg("Config", "invalid cache path - ", val); return 1;}
   strcpy(fn, val);

   if ((val = cFile->GetWord()))
      {if (strcmp("xa", val))
          {Say.Emsg("Config","invalid cache option - ",val); return 1;}
          else isxa = 1;
      }

   if (fn[k-1] != '*')
      {for (i = k-1; i; i--) if (fn[i] != '/') break;
       fn[i+1] = '/'; fn[i+2] = '\0';
       xcacheBuild(grp, fn, isxa);
       return 0;
      }

   for (i = k-1; i; i--) if (fn[i] == '/') break;
   i++; strcpy(dn, &fn[i]); fn[i] = '\0';
   sfxdir = &fn[i]; pfxdir = dn; pfxln = strlen(dn)-1;
   if (!(DFD = opendir(fn)))
      {Say.Emsg("Config", errno, "open cache directory", fn); return 1;}

   errno = 0;
   while((dp = readdir(DFD)))
        {if (!strcmp(dp->d_name, ".") || !strcmp(dp->d_name, "..")
         || (pfxln && strncmp(dp->d_name, pfxdir, pfxln)))
            continue;
         strcpy(sfxdir, dp->d_name);
         if (stat(fn, &buff)) break;
         if ((buff.st_mode & S_IFMT) == S_IFDIR)
            {val = sfxdir + strlen(sfxdir) - 1;
            if (*val++ != '/') {*val++ = '/'; *val = '\0';}
            xcacheBuild(grp, fn, isxa);
            cnum++;
            }
         errno = 0;
        }

   if ((rc = errno))
      Say.Emsg("Config", errno, "process cache directory", fn);
      else if (!cnum) Say.Say("Config warning: no cache directories found in ",val);

   closedir(DFD);
   return rc != 0;
}

void XrdFrmConfig::xcacheBuild(char *grp, char *fn, int isxa)
{
   struct VPInfo *nP = VPList;
   XrdOucTList *tP;

   while(nP && strcmp(nP->Name, grp)) nP = nP->Next;

   if (!nP) VPList = nP = new VPInfo(grp, 0, VPList);

   tP = nP->Dir;
   while(tP && strcmp(tP->text, fn)) tP = tP->next;
   if (!tP) nP->Dir = new XrdOucTList(fn, isxa, nP->Dir);
}
  
/******************************************************************************/
/* Private:                        x d p o l                                  */
/******************************************************************************/
  

/* Function: xdpol

   Purpose:  To parse the directive: dirpolicy <sec>

             <sec>     number of seconds to hold an empty directory or the
                       word 'forever'.

   Output: 0 upon success or !0 upon failure.
*/
int XrdFrmConfig::xdpol()
{   int htm;
    char *val;

    if (!(val = cFile->GetWord()))
       {Say.Emsg("Config",  "dirpolicy hold time not specified"); return 1;}
    if (XrdOuca2x::a2tm(Say,"dirpolicy hold time", val, &htm, 0)) return 1;
    dirHold = htm;
    return 0;
}

/******************************************************************************/
/* Private:                        x m a x x                                  */
/******************************************************************************/

/* Function: xmaxx

   Purpose:  To parse the directive: xfrmax <num>

             <num>     maximum number of simultaneous transfers

   Output: 0 upon success or !0 upon failure.
*/
int XrdFrmConfig::xmaxx()
{   int xmax = 1;
    char *val;

    if (!(val = cFile->GetWord()))
       {Say.Emsg("Config", "xfrmax value not specified"); return 1;}
    if (XrdOuca2x::a2i(Say, "xfrmax", val, &xmax, 1)) return 1;
    xfrMax = xmax;
    return 0;
}


/******************************************************************************/
/*                                  x m o n                                   */
/******************************************************************************/

/* Function: xmon

   Purpose:  Parse directive: monitor [all] [mbuff <sz>] 
                                      [flush <sec>] [window <sec>]
                                      dest [Events] <host:port>

   Events: [files] [info] [io] [stage] [user] <host:port>

         all                enables monitoring for all connections.
         mbuff  <sz>        size of message buffer.
         flush  <sec>       time (seconds, M, H) between auto flushes.
         window <sec>       time (seconds, M, H) between timing marks.
         dest               specified routing information. Up to two dests
                            may be specified.
         files              only monitors file open/close events.
         info               monitors client appid and info requests.
         io                 monitors I/O requests, and files open/close events.
         stage              monitors file stage operations
         user               monitors user login and disconnect events.
         <host:port>        where monitor records are to be sentvia UDP.

   Output: 0 upon success or !0 upon failure. Ignored by master.
*/
int XrdFrmConfig::xmon()
{   char  *val, *cp, *monDest[2] = {0, 0};
    long long tempval;
    int i, monFlush=0, monMBval=0, monWWval=0, xmode=0, monMode[2] = {0, 0};

    while((val = cFile->GetWord()))

         {     if (!strcmp("all",  val)) xmode = XROOTD_MON_ALL;
          else if (!strcmp("flush", val))
                {if (!(val = cFile->GetWord()))
                    {Say.Emsg("Config", "monitor flush value not specified");
                     return 1;
                    }
                 if (XrdOuca2x::a2tm(Say,"monitor flush",val,
                                         &monFlush,1)) return 1;
                }
          else if (!strcmp("mbuff",val))
                  {if (!(val = cFile->GetWord()))
                      {Say.Emsg("Config", "monitor mbuff value not specified");
                       return 1;
                      }
                   if (XrdOuca2x::a2sz(Say,"monitor mbuff", val,
                                           &tempval, 1024, 65536)) return 1;
                    monMBval = static_cast<int>(tempval);
                  }
          else if (!strcmp("window", val))
                {if (!(val = cFile->GetWord()))
                    {Say.Emsg("Config", "monitor window value not specified");
                     return 1;
                    }
                 if (XrdOuca2x::a2tm(Say,"monitor window",val,
                                         &monWWval,1)) return 1;
                }
          else break;
         }

    if (!val) {Say.Emsg("Config", "monitor dest not specified"); return 1;}

    for (i = 0; i < 2; i++)
        {if (strcmp("dest", val)) break;
         while((val = cFile->GetWord()))
                   if (!strcmp("files",val)
                   ||  !strcmp("info", val)
                   ||  !strcmp("io",   val)
                   ||  !strcmp("user", val)) {}
              else if (!strcmp("stage",val)) monMode[i] |=  XROOTD_MON_STAGE;
              else break;
          if (!val) {Say.Emsg("Config","monitor dest value not specified");
                     return 1;
                    }
          if (!(cp = index(val, (int)':')) || !atoi(cp+1))
             {Say.Emsg("Config","monitor dest port missing or invalid in",val);
              return 1;
             }
          monDest[i] = strdup(val);
         if (!(val = cFile->GetWord())) break;
        }

    if (val)
       {if (!strcmp("dest", val))
           Say.Emsg("Config", "Warning, a maximum of two dest values allowed.");
           else Say.Emsg("Config", "Warning, invalid monitor option", val);
       }

// Make sure dests differ
//
   if (monDest[0] && monDest[1] && !strcmp(monDest[0], monDest[1]))
      {Say.Emsg("Config", "Warning, monitor dests are identical.");
       monMode[0] |= monMode[1]; monMode[1] = 0;
       free(monDest[1]); monDest[1] = 0;
      }

// Don't bother doing any more if staging is not enabled
//
   if (!monMode[0] && !monMode[1]) return 0;
   monStage = 1;

// Set the monitor defaults
//
   XrdXrootdMonitor::Defaults(monMBval, monWWval, monFlush);
   if (monDest[0]) monMode[0] |= (monMode[0] ? XROOTD_MON_FILE|xmode : xmode);
   if (monDest[1]) monMode[1] |= (monMode[1] ? XROOTD_MON_FILE|xmode : xmode);
   XrdXrootdMonitor::Defaults(monDest[0],monMode[0],monDest[1],monMode[1]);
   return 0;
}

/******************************************************************************/
/* Private:                         x n m l                                   */
/******************************************************************************/

/* Function: xnml

   Purpose:  To parse the directive: namelib <path> [<parms>]

             <path>    the path of the filesystem library to be used.
             <parms>   optional parms to be passed

  Output: 0 upon success or !0 upon failure.
*/

int XrdFrmConfig::xnml()
{
    char *val, parms[1024];

// Get the path
//
   if (!(val = cFile->GetWord()) || !val[0])
      {Say.Emsg("Config", "namelib not specified"); return 1;}

// Record the path
//
   if (N2N_Lib) free(N2N_Lib);
   N2N_Lib = strdup(val);

// Record any parms
//
   if (!cFile->GetRest(parms, sizeof(parms)))
      {Say.Emsg("Config", "namelib parameters too long"); return 1;}
   if (N2N_Parms) free(N2N_Parms);
   N2N_Parms = (*parms ? strdup(parms) : 0);
   return 0;
}

/******************************************************************************/
/* Private:                         x p o l                                   */
/******************************************************************************/

/* Function: xpol

   Purpose:  To parse the directive: policy {*|sname} {nopurge|min [max]] [opts]

             *         The default policy for all spaces.

             sname     The policy to apply for this space. Defaults apply for
                       unspecified values. To make sure the specified default
                       is used, the '*' entry must appear first.

             nopurge   Turns off purging.

             min%      Minimum free space; purge starts when less available.
                       Can be specified as a percentage (i.e., n%) or an
                       absolute size value (with k, m, g, t suffix).
                       Default: 5%

             max%      Maximum free space; purge stops  when more available.
                       Must be specified in the same units as min and must be
                       greater than min.
                       Default: min% + 2 or min * 1.2

       opts: hold <tm> Time to hold a file before it can be purged. The <tm>
                       can be a suffixed number or 'forever'.
                       Default: 20h (20*3600)s

             polprog   Invoke the policy program to do final determination.


   Output: 0 upon success or !0 upon failure.
*/
int XrdFrmConfig::xpol()
{
   Policy *pP = &dfltPolicy;
   char *val, sname[XrdOssSpace::minSNbsz];
   long long minP = dfltPolicy.minFree, maxP = dfltPolicy.maxFree;
   int       Hold = dfltPolicy.Hold, Ext = 0;
   struct purgeopts {const char *opname; int isTime; int *oploc;} pgopts[] =
      {
       {"polprog", -1, &Ext},
       {"hold",     1, &Hold}
      };
   int i, rc, numopts = sizeof(pgopts)/sizeof(struct purgeopts);

// Get the space name
//
   if (!(val = cFile->GetWord()))
      {Say.Emsg("Config", "space name not specified"); return 1;}
   if (strlen(val) >= sizeof(sname))
      {Say.Emsg("Config", "space name '", val, "' too long"); return 1;}

// If we have an equal sign then an external policy is being defined
//
   if (!strcmp("=", val)) return xpolprog();
   strcpy(sname, val);

// The next item may be minimum percentage followed by a maximum percentage
// Otherwise, it may be 'nopurge'.
//
   if (    (val = cFile->GetWord()) && isdigit(*val))
      {if (    XrdOuca2x::a2sp(Say, "min free", val, &minP, 1)) return 1;
       if ((val = cFile->GetWord()) && isdigit(*val))
          {if (XrdOuca2x::a2sp(Say, "max free", val, &maxP, 1)) return 1;
           if ((minP < 0 && maxP >= 0) || (minP >= 0 && maxP < 0))
              {Say.Emsg("Config", "purge min/max may not differ in type.");
               return 1;
              }
           if (XRDABS(minP) >= XRDABS(maxP))
              {Say.Emsg("Config", "purge min must be < max value."); return 1;}
           val = cFile->GetWord();
          } else {
           if (minP < 0) maxP = (minP < -99 ? -100 : minP - 1);
              else       maxP = (minP * 120LL)/100LL;
          }
      } else if (val && !strcmp(val, "nopurge"))
                {minP = maxP = 0;
                 if ((val = cFile->GetWord()))
                    {Say.Say("Config warning: ignoring extraneous policy option '",val,"'.");
                     val = 0;
                    }
                }

// Pick up the remining options
//
   while(val)
        {for (i = 0; i < numopts; i++) if (!strcmp(val,pgopts[i].opname)) break;
         if (i >= numopts)
            {Say.Say("Config warning: ignoring invalid policy option '",val,"'.");
             val = cFile->GetWord();
             continue;
            }
         if (pgopts[i].isTime < 0) *(pgopts[i].oploc) = 1;
            else {if (!(val = cFile->GetWord()))
                     {Say.Emsg("Config", "policy", pgopts[i].opname,
                                         "argument not specified.");
                      return 1;
                     }
                  rc = (pgopts[i].isTime
                     ?         getTime(    "purge value",val,pgopts[i].oploc,0)
                     : XrdOuca2x::a2i (Say,"purge value",val,pgopts[i].oploc,0));
                  if (rc) return 1;
                 }
         val = cFile->GetWord();
        }

// If an external policy applies, it must be present
//
   if (Ext && !pProg)
      {Say.Emsg("Config", "External policy has not been pre-defined.");
       return 1;
      }

// Add this policy definition
//
   while(pP && strcmp(pP->Sname, sname)) pP = pP->Next;
   if (pP) {pP->minFree=minP; pP->maxFree=maxP; pP->Hold=Hold; pP->Ext=Ext;}
      else {pP = new Policy(sname, minP, maxP, Hold, Ext);
            pP->Next = dfltPolicy.Next; dfltPolicy.Next = pP;
           }
    return 0;
}

/******************************************************************************/
/* Private:                     x p o l p r o g                               */
/******************************************************************************/
  
/* Function: xpolprog

   Purpose:  To parse the directive: policy = [vars] |<prog> [args]

   Where:
             =         Defines an external policy via a program, as follows:

             vars      The information to ship to the program via stdin:
                       atime   - access time
                       ctime   - create time
                       fname   - the filename itself
                       fsize   - file size
                       fspace  - free  space
                       mtime   - modify time
                       pfn     - physical file name
                       sname   - space name
                       tspace  - total space

             |<prog>   The name of the policy program to receive the info.

             args      Optional program arguments (substituted), up to 8.

   Output: 0 upon success or !0 upon failure.
*/
int XrdFrmConfig::xpolprog()
{
   char *val, pBuff[4096], *pbP = pBuff;
   struct polopts {const char *opname; int opval;} plopts[] =
      {
       {"atime",  PP_atime },
       {"ctime",  PP_ctime },
       {"fname",  PP_fname },
       {"fsize",  PP_fsize },
       {"fspace", PP_fspace},
       {"mtime",  PP_mtime },
       {"pfn",    PP_pfn   },
       {"sname",  PP_sname },
       {"tspace", PP_tspace},
       {"usage",  PP_usage}
      };
   int i, n, numopts = sizeof(plopts)/sizeof(struct polopts);

// Get the first token
//
   if (!(val = cFile->GetWord()))
      {Say.Emsg("Config", "policy program not specified"); return 1;}
   pVecNum = 0;

// Pick up the remining options
//
   while(val && *val != '|')
        {for (i = 0; i < numopts; i++) if (!strcmp(val,plopts[i].opname)) break;
         if (i >= numopts)
            {Say.Say("Config warning: ignoring invalid policy option '",val,"'.");
             val = cFile->GetWord();
             continue;
            }
         if (pVecNum >= pVecMax)
            {Say.Emsg("Config", "To many policy program variables specified.");
             return 1;
            }
         pVec[pVecNum++] = static_cast<char>(plopts[i].opval);
         val = cFile->GetWord();
        }

// Pick up the program
//
   if (val) val++;
   if (val && !(*val)) val = cFile->GetWord();
   if (!val)
      {Say.Emsg("Config", "policy program not specified."); return 1;}
   i = strlen(val);
   if (i >= (int)sizeof(pBuff)-8)
      {Say.Emsg("Config", "policy program name is too long."); return 1;}
   strcpy(pBuff, val); pbP = pBuff+i; *(pbP+1) = '\0';

// Now get any optional arguments
//
   n = sizeof(pBuff) - i - 1;
   if (!cFile->GetRest(pbP+1, n))
      {Say.Emsg("Config", "policy program args are too long."); return 1;}
   if (*(pbP+1)) *pbP = ' ';

// Record the program
//
   if (pProg) free(pProg);
   pProg = strdup(pBuff);
   return 0;
}

/******************************************************************************/
/* Private:                         x w t m                                   */
/******************************************************************************/

/* Function: xwtm

   Purpose:  To parse the directive: waittime <sec>

             <sec>     number of seconds between scans.

   Output: 0 upon success or !0 upon failure.
*/
int XrdFrmConfig::xwtm()
{   int wscan = 0, wtime = (Test ? 1 : 30);
    char *val;

    if (!(val = cFile->GetWord()))
       {Say.Emsg("Config", "wait time not specified"); return 1;}
    if (XrdOuca2x::a2tm(Say, "wait time", val, &wscan, wtime)) return 1;
    WaitTime = wscan;
    return 0;
}
