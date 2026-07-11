# RMDB only uses GoogleTest; GoogleMock and the upstream self-tests are not
# needed for the course unit tests.
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
ADD_SUBDIRECTORY(googletest)
