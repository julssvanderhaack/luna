# cmake -DDIR=… -P fresh_home.cmake: an empty folder for one test's HOME.
file(REMOVE_RECURSE "${DIR}")
file(MAKE_DIRECTORY "${DIR}")
