# CMAKE generated file: DO NOT EDIT!
# Generated by "Unix Makefiles" Generator, CMake Version 3.14

# Delete rule output on recipe failure.
.DELETE_ON_ERROR:


#=============================================================================
# Special targets provided by cmake.

# Disable implicit rules so canonical targets will work.
.SUFFIXES:


# Remove some rules from gmake that .SUFFIXES does not remove.
SUFFIXES =

.SUFFIXES: .hpux_make_needs_suffix_list


# Suppress display of executed commands.
$(VERBOSE).SILENT:


# A target that is always out of date.
cmake_force:

.PHONY : cmake_force

#=============================================================================
# Set environment variables for the build.

# The shell in which to execute make rules.
SHELL = /bin/sh

# The CMake executable.
CMAKE_COMMAND = /usr/bin/cmake

# The command to remove a file.
RM = /usr/bin/cmake -E remove -f

# Escaping for special characters.
EQUALS = =

# The top-level source directory on which CMake was run.
CMAKE_SOURCE_DIR = /home/matthias/Dokumente/Studium_Karlsruhe/Semester11/Masterarbeit/DistributedStringSorting/distributed-string-sorting/KaDiS

# The top-level build directory on which CMake was run.
CMAKE_BINARY_DIR = /home/matthias/Dokumente/Studium_Karlsruhe/Semester11/Masterarbeit/DistributedStringSorting/distributed-string-sorting/KaDiS/build

# Utility rule file for AnalyzeAllSources.

# Include the progress variables for this target.
include CMakeFiles/AnalyzeAllSources.dir/progress.make

CMakeFiles/AnalyzeAllSources:
	perl /home/matthias/Dokumente/Studium_Karlsruhe/Semester11/Masterarbeit/DistributedStringSorting/distributed-string-sorting/KaDiS/codestyle/analyze-source.pl -aw

AnalyzeAllSources: CMakeFiles/AnalyzeAllSources
AnalyzeAllSources: CMakeFiles/AnalyzeAllSources.dir/build.make

.PHONY : AnalyzeAllSources

# Rule to build all files generated by this target.
CMakeFiles/AnalyzeAllSources.dir/build: AnalyzeAllSources

.PHONY : CMakeFiles/AnalyzeAllSources.dir/build

CMakeFiles/AnalyzeAllSources.dir/clean:
	$(CMAKE_COMMAND) -P CMakeFiles/AnalyzeAllSources.dir/cmake_clean.cmake
.PHONY : CMakeFiles/AnalyzeAllSources.dir/clean

CMakeFiles/AnalyzeAllSources.dir/depend:
	cd /home/matthias/Dokumente/Studium_Karlsruhe/Semester11/Masterarbeit/DistributedStringSorting/distributed-string-sorting/KaDiS/build && $(CMAKE_COMMAND) -E cmake_depends "Unix Makefiles" /home/matthias/Dokumente/Studium_Karlsruhe/Semester11/Masterarbeit/DistributedStringSorting/distributed-string-sorting/KaDiS /home/matthias/Dokumente/Studium_Karlsruhe/Semester11/Masterarbeit/DistributedStringSorting/distributed-string-sorting/KaDiS /home/matthias/Dokumente/Studium_Karlsruhe/Semester11/Masterarbeit/DistributedStringSorting/distributed-string-sorting/KaDiS/build /home/matthias/Dokumente/Studium_Karlsruhe/Semester11/Masterarbeit/DistributedStringSorting/distributed-string-sorting/KaDiS/build /home/matthias/Dokumente/Studium_Karlsruhe/Semester11/Masterarbeit/DistributedStringSorting/distributed-string-sorting/KaDiS/build/CMakeFiles/AnalyzeAllSources.dir/DependInfo.cmake --color=$(COLOR)
.PHONY : CMakeFiles/AnalyzeAllSources.dir/depend

