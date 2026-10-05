# Guitar Pro song files for the optional song palette. Non-real-time, JUCE-free; needs zlib for the
# deflated entries of .gp (ZIP) archives.
find_package(ZLIB REQUIRED)

add_library(bass2midi_song STATIC
    src/SongLoader.cpp
    src/SongTimeline.cpp
    src/GpifReader.cpp
    src/Gp5Reader.cpp
    src/ZipReader.cpp
    src/MiniXml.cpp)
target_include_directories(bass2midi_song PUBLIC include)
target_link_libraries(bass2midi_song PUBLIC bass2midi_dsp PRIVATE ZLIB::ZLIB bass2midi_warnings)
