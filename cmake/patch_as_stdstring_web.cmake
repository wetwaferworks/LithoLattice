# 2026-11-16 (LK_WEB only): writes the web build's copy of AngelScript's scriptstdstring add-on.
# LithoRoot has no <regex> (and JD's rule is no third-party regex engine), so string::regexFind
# stays registered but becomes a WEB-ONLY FAIL-SAFE STUB: it logs once and returns no match.
# No shipped script, mod or test calls regexFind (grep 2026-11-16: 0 uses). The pinned SDK
# source is never modified; native builds compile it as-is. Usage:
#   cmake -DIN=<sdk>/add_on/scriptstdstring/scriptstdstring.cpp -DOUT=<copy> -P patch_as_stdstring_web.cmake
file(READ "${IN}" src)
set(inc "#include <regex>")
string(FIND "${src}" "${inc}" at)
if(at LESS 0)
  message(FATAL_ERROR "patch_as_stdstring_web: '#include <regex>' not found in ${IN} (add-on changed?)")
endif()
string(REPLACE "${inc}" "#include <atomic> // WEB-ONLY FAIL-SAFE STUB: <regex> removed (LithoKraft LK_WEB)" src "${src}")
set(b "std::regex pattern(rex, std::regex_constants::ECMAScript | std::regex_constants::collate);")
set(e "return (int)match.prefix().length();")
string(FIND "${src}" "${b}" ib)
string(FIND "${src}" "${e}" ie)
if(ib LESS 0 OR ie LESS ib)
  message(FATAL_ERROR "patch_as_stdstring_web: StringRegexFind body not found in ${IN} (add-on changed?)")
endif()
string(LENGTH "${e}" le)
math(EXPR tail "${ie} + ${le}")
string(SUBSTRING "${src}" 0 ${ib} head)
string(SUBSTRING "${src}" ${tail} -1 rest)
set(stub "// WEB-ONLY FAIL-SAFE STUB (LithoKraft LK_WEB, 2026-11-16): no regex engine in the web build.
	// Logs once, then always reports no match, the same result as a pattern that matches nothing.
	(void)rex;
	static std::atomic<bool> warned(false);
	if (!warned.exchange(true))
		fprintf(stderr, \"[script] string::regexFind is not available in the web build; it returns no match (-1)\\n\");
	outLengthOfMatch = 0;
	return -1;")
set(src "${head}${stub}${rest}")
foreach(gone "#include <regex>" "regex_search" "std::cmatch")  # the comments above the body may still say std::regex
  string(FIND "${src}" "${gone}" left)
  if(NOT left LESS 0)
    message(FATAL_ERROR "patch_as_stdstring_web: '${gone}' still present after the patch")
  endif()
endforeach()
file(WRITE "${OUT}.tmp" "${src}")
file(RENAME "${OUT}.tmp" "${OUT}")
