solution "EC"
	language "C++"
	configurations { "Debug" }
	
	configuration "Debug"
		optimize "Off"
		flags "Symbols"

	project "EC"
		kind "ConsoleApp"
		files { "**.h", "**.cpp" }
