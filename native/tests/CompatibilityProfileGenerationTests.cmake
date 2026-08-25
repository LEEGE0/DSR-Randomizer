if(NOT EXISTS "${PROFILE_JSON}" OR NOT EXISTS "${GENERATED_HEADER}")
  message(FATAL_ERROR "Profile generation inputs are missing")
endif()

file(SHA256 "${PROFILE_JSON}" actual_sha256)
file(READ "${GENERATED_HEADER}" generated_header)
string(FIND "${generated_header}" "\"${actual_sha256}\"" hash_offset)
if(hash_offset EQUAL -1)
  message(FATAL_ERROR
    "Generated native compatibility constants are stale for the source JSON")
endif()
