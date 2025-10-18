#if defined(APP_HUB)
  #include "hub/main.cpp"
#elif defined(APP_CAM)
  #include "cam/main.cpp"
#else
  #error "Define APP_HUB or APP_CAM via build_flags in platformio.ini"
#endif

