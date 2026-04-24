
function(automaticGameFolderOutput fallout4_mods_output)

    if(DEFINED ENV{FALLOUT4_FOLDER} AND IS_DIRECTORY "$ENV{FALLOUT4_FOLDER}/Data")
        copyOutputs("$ENV{FALLOUT4_FOLDER}/Data")
    endif()
    if(DEFINED ENV{FALLOUT4_MODS_FOLDER} AND IS_DIRECTORY "$ENV{FALLOUT4_MODS_FOLDER}" AND fallout4_mods_output)
        copyOutputs("$ENV{FALLOUT4_MODS_FOLDER}/${BEAUTIFUL_NAME}${PROJECT_SUFFIX}")
    endif()
    if(DEFINED ENV{FALLOUT4_MODS_FOLDER2} AND IS_DIRECTORY "$ENV{FALLOUT4_MODS_FOLDER2}" AND fallout4_mods_output)
        copyOutputs("$ENV{FALLOUT4_MODS_FOLDER2}/${BEAUTIFUL_NAME}${PROJECT_SUFFIX}")
    endif()

endfunction()
