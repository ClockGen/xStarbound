#include "StarText.hpp"

#include <regex>

namespace Star {

namespace Text {
  static auto stripEscapeRegex = std::regex(strf("\\{:c}[^;]*{:c}", CmdEsc, EndEsc));
  String stripEscapeCodes(String const& s) {
    return std::regex_replace(s.utf8(), stripEscapeRegex, "");
  }

  static std::string escapeChars = strf("{:c}{:c}", CmdEsc, StartEsc);

  bool processText(StringView text, TextCallback textFunc, CommandsCallback commandsFunc, bool includeCommandSides) {
    std::string_view escChars(escapeChars);

    std::string_view str = text.utf8();
    while (true) {
      size_t escape = str.find_first_of(escChars);
      if (escape != NPos) {
        escape = str.find_first_not_of(escChars, escape) - 1; // jump to the last ^

        size_t end = str.find_first_of(EndEsc, escape);
        if (end != NPos) {
          if (escape && !textFunc(str.substr(0, escape)))
            return false;
          if (commandsFunc) {
            StringView commands = includeCommandSides
              ? str.substr(escape, end - escape + 1)
              : str.substr(escape + 1, end - escape - 1);
            if (!commands.empty() && !commandsFunc(commands))
              return false;
          }
          str = str.substr(end + 1);
          continue;
        }
      }

      if (!str.empty())
        return textFunc(str);

      return true;
    }
  }

  // The below two functions aren't used anymore, not bothering with StringView for them
  String preprocessEscapeCodes(String const& s) {
    bool escape = false;
    std::string result = s.utf8();

    size_t escapeStartIdx = 0;
    for (size_t i = 0; i < result.size(); i++) {
      auto& c = result[i];
      if (isEscapeCode(c)) {
        escape = true;
        escapeStartIdx = i;
      }
      if ((c <= SpecialCharLimit) && !(c == StartEsc))
        escape = false;
      if ((c == EndEsc) && escape)
        result[escapeStartIdx] = StartEsc;
    }
    return {result};
  }

  String extractCodes(String const& s) {
    bool escape = false;
    StringList result;
    String escapeCode;
    for (auto c : preprocessEscapeCodes(s)) {
      if (c == StartEsc)
        escape = true;
      if (c == EndEsc) {
        escape = false;
        for (auto command : escapeCode.split(','))
          result.append(command);
        escapeCode = "";
      }
      if (escape && (c != StartEsc))
        escapeCode.append(c);
    }
    if (!result.size())
      return "";
    return "^" + result.join(",") + ";";
  }
}

}