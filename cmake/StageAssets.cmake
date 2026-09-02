if(NOT DEFINED IMAO_SOURCE_ASSETS OR NOT DEFINED IMAO_DESTINATION_ROOT OR NOT DEFINED IMAO_CONFIGURATION)
    message(FATAL_ERROR "StageAssets.cmake requires source, destination, and configuration variables.")
endif()

set(destination "${IMAO_DESTINATION_ROOT}/Assets")
file(MAKE_DIRECTORY "${destination}")

if(IMAO_CONFIGURATION STREQUAL "Release")
    # Remove an XML left by an older build, then stage every asset except the
    # 460 MiB base-map XML source. Optional pack XML files remain supported.
    file(REMOVE "${destination}/FeaturesDatas/Map_features.yml")
    file(COPY "${IMAO_SOURCE_ASSETS}/" DESTINATION "${destination}"
        PATTERN "Map_features.yml" EXCLUDE)
else()
    file(COPY "${IMAO_SOURCE_ASSETS}/" DESTINATION "${destination}")
endif()
