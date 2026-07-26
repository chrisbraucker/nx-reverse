#include <switch.h>

#include "logger.hpp"

namespace {

bool g_romfs_initialized = false;
bool g_pl_initialized = false;
bool g_setsys_initialized = false;
bool g_set_initialized = false;
bool g_psm_initialized = false;
bool g_nifm_initialized = false;
bool g_lbl_initialized = false;

void LogInitializationResult(const char *name, Result rc) {
  requester::logger::Bootstrap("borealis platform: %s rc=0x%08X", name, rc);
}

} // namespace

extern "C" void userAppInit(void) {
  Result rc = romfsInit();
  g_romfs_initialized = R_SUCCEEDED(rc);
  LogInitializationResult("romfsInit", rc);

  rc = plInitialize(PlServiceType_User);
  g_pl_initialized = R_SUCCEEDED(rc);
  LogInitializationResult("plInitialize", rc);

  rc = setsysInitialize();
  g_setsys_initialized = R_SUCCEEDED(rc);
  LogInitializationResult("setsysInitialize", rc);

  rc = setInitialize();
  g_set_initialized = R_SUCCEEDED(rc);
  LogInitializationResult("setInitialize", rc);

  rc = psmInitialize();
  g_psm_initialized = R_SUCCEEDED(rc);
  LogInitializationResult("psmInitialize", rc);

  rc = nifmInitialize(NifmServiceType_User);
  g_nifm_initialized = R_SUCCEEDED(rc);
  LogInitializationResult("nifmInitialize", rc);

  rc = lblInitialize();
  g_lbl_initialized = R_SUCCEEDED(rc);
  LogInitializationResult("lblInitialize", rc);
}

extern "C" void userAppExit(void) {
  if (g_lbl_initialized) {
    lblExit();
  }
  if (g_nifm_initialized) {
    nifmExit();
  }
  if (g_psm_initialized) {
    psmExit();
  }
  if (g_set_initialized) {
    setExit();
  }
  if (g_setsys_initialized) {
    setsysExit();
  }
  if (g_pl_initialized) {
    plExit();
  }
  if (g_romfs_initialized) {
    romfsExit();
  }
}
