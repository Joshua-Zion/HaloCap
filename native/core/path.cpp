#include "path.h"

#include <cctype>
#include <cmath>

namespace halocap::core {

namespace {

// 手写数字读取：strtod 依赖 locale 的小数点，而 path 数据里固定是 '.'。
struct Reader {
    std::string_view text;
    std::size_t pos = 0;

    bool Eof() const { return pos >= text.size(); }

    void SkipSeparators() {
        while (pos < text.size()) {
            const char c = text[pos];
            if (c == ' ' || c == ',' || c == '\t' || c == '\r' || c == '\n') {
                ++pos;
            } else {
                break;
            }
        }
    }

    bool PeekLetter(char* letter) const {
        std::size_t index = pos;
        while (index < text.size() && (text[index] == ' ' || text[index] == ',' ||
                                       text[index] == '\t' || text[index] == '\r' ||
                                       text[index] == '\n')) {
            ++index;
        }
        if (index >= text.size() || !std::isalpha(static_cast<unsigned char>(text[index]))) {
            return false;
        }
        *letter = text[index];
        return true;
    }

    void AdvanceToLetter() {
        while (pos < text.size() && !std::isalpha(static_cast<unsigned char>(text[pos]))) {
            ++pos;
        }
    }

    bool ReadNumber(float* value) {
        SkipSeparators();
        if (pos >= text.size()) {
            return false;
        }
        std::size_t index = pos;
        bool negative = false;
        if (text[index] == '+' || text[index] == '-') {
            negative = text[index] == '-';
            ++index;
        }
        double number = 0.0;
        bool digits = false;
        while (index < text.size() && std::isdigit(static_cast<unsigned char>(text[index]))) {
            number = number * 10.0 + (text[index] - '0');
            digits = true;
            ++index;
        }
        if (index < text.size() && text[index] == '.') {
            ++index;
            double scale = 0.1;
            while (index < text.size() &&
                   std::isdigit(static_cast<unsigned char>(text[index]))) {
                number += (text[index] - '0') * scale;
                scale *= 0.1;
                digits = true;
                ++index;
            }
        }
        if (!digits) {
            return false;
        }
        if (index < text.size() && (text[index] == 'e' || text[index] == 'E')) {
            ++index;
            bool exponentNegative = false;
            if (index < text.size() && (text[index] == '+' || text[index] == '-')) {
                exponentNegative = text[index] == '-';
                ++index;
            }
            int exponent = 0;
            while (index < text.size() &&
                   std::isdigit(static_cast<unsigned char>(text[index]))) {
                exponent = exponent * 10 + (text[index] - '0');
                ++index;
            }
            number *= std::pow(10.0, exponentNegative ? -exponent : exponent);
        }
        pos = index;
        *value = static_cast<float>(negative ? -number : number);
        return true;
    }
};

void Push(std::vector<PathCommand>* commands, PathCommand::Kind kind, Point a, Point b,
          Point c) {
    PathCommand command;
    command.kind = kind;
    command.points[0] = a;
    command.points[1] = b;
    command.points[2] = c;
    commands->push_back(command);
}

}  // namespace

bool ParsePathData(std::string_view data, std::vector<PathCommand>* commands, bool* evenOdd) {
    if (commands == nullptr) {
        return false;
    }
    commands->clear();
    if (evenOdd != nullptr) {
        *evenOdd = false;
    }

    Reader reader{ data, 0 };
    reader.SkipSeparators();
    // WPF 的填充规则前缀：F0 = EvenOdd，F1 = Nonzero。
    if (reader.pos + 1 < data.size() && (data[reader.pos] == 'F' || data[reader.pos] == 'f') &&
        std::isdigit(static_cast<unsigned char>(data[reader.pos + 1]))) {
        if (evenOdd != nullptr) {
            *evenOdd = data[reader.pos + 1] == '0';
        }
        reader.pos += 2;
    }

    Point current{ 0.0f, 0.0f };
    Point start{ 0.0f, 0.0f };
    Point lastControl{ 0.0f, 0.0f };
    char previousCommand = 0;
    char command = 0;

    while (true) {
        reader.SkipSeparators();
        if (reader.Eof()) {
            break;
        }
        const char next = data[reader.pos];
        if (std::isalpha(static_cast<unsigned char>(next))) {
            command = next;
            ++reader.pos;
        } else if (command == 0) {
            return false;
        }
        // 没有新字母就是隐式重复上一条；M 之后的重复按 L 处理（WPF/SVG 同规则）。
        else if (command == 'M') {
            command = 'L';
        } else if (command == 'm') {
            command = 'l';
        }

        const bool relative = std::islower(static_cast<unsigned char>(command)) != 0;
        const char op = static_cast<char>(std::toupper(static_cast<unsigned char>(command)));

        switch (op) {
        case 'M': {
            float x = 0.0f;
            float y = 0.0f;
            if (!reader.ReadNumber(&x) || !reader.ReadNumber(&y)) {
                return false;
            }
            current = relative ? Point{ current.x + x, current.y + y } : Point{ x, y };
            start = current;
            Push(commands, PathCommand::Kind::Move, current, {}, {});
            break;
        }
        case 'L': {
            float x = 0.0f;
            float y = 0.0f;
            if (!reader.ReadNumber(&x) || !reader.ReadNumber(&y)) {
                return false;
            }
            current = relative ? Point{ current.x + x, current.y + y } : Point{ x, y };
            Push(commands, PathCommand::Kind::Line, current, {}, {});
            break;
        }
        case 'H': {
            float x = 0.0f;
            if (!reader.ReadNumber(&x)) {
                return false;
            }
            current = Point{ relative ? current.x + x : x, current.y };
            Push(commands, PathCommand::Kind::Line, current, {}, {});
            break;
        }
        case 'V': {
            float y = 0.0f;
            if (!reader.ReadNumber(&y)) {
                return false;
            }
            current = Point{ current.x, relative ? current.y + y : y };
            Push(commands, PathCommand::Kind::Line, current, {}, {});
            break;
        }
        case 'C': {
            Point c1{};
            Point c2{};
            Point end{};
            if (!reader.ReadNumber(&c1.x) || !reader.ReadNumber(&c1.y) ||
                !reader.ReadNumber(&c2.x) || !reader.ReadNumber(&c2.y) ||
                !reader.ReadNumber(&end.x) || !reader.ReadNumber(&end.y)) {
                return false;
            }
            if (relative) {
                c1 = Point{ current.x + c1.x, current.y + c1.y };
                c2 = Point{ current.x + c2.x, current.y + c2.y };
                end = Point{ current.x + end.x, current.y + end.y };
            }
            Push(commands, PathCommand::Kind::Cubic, c1, c2, end);
            lastControl = c2;
            current = end;
            break;
        }
        case 'S': {
            Point c2{};
            Point end{};
            if (!reader.ReadNumber(&c2.x) || !reader.ReadNumber(&c2.y) ||
                !reader.ReadNumber(&end.x) || !reader.ReadNumber(&end.y)) {
                return false;
            }
            if (relative) {
                c2 = Point{ current.x + c2.x, current.y + c2.y };
                end = Point{ current.x + end.x, current.y + end.y };
            }
            const bool smooth = previousCommand == 'C' || previousCommand == 'S';
            const Point c1 = smooth ? Point{ 2.0f * current.x - lastControl.x,
                                             2.0f * current.y - lastControl.y }
                                    : current;
            Push(commands, PathCommand::Kind::Cubic, c1, c2, end);
            lastControl = c2;
            current = end;
            break;
        }
        case 'Q': {
            Point c{};
            Point end{};
            if (!reader.ReadNumber(&c.x) || !reader.ReadNumber(&c.y) ||
                !reader.ReadNumber(&end.x) || !reader.ReadNumber(&end.y)) {
                return false;
            }
            if (relative) {
                c = Point{ current.x + c.x, current.y + c.y };
                end = Point{ current.x + end.x, current.y + end.y };
            }
            Push(commands, PathCommand::Kind::Quad, c, {}, end);
            lastControl = c;
            current = end;
            break;
        }
        case 'T': {
            Point end{};
            if (!reader.ReadNumber(&end.x) || !reader.ReadNumber(&end.y)) {
                return false;
            }
            if (relative) {
                end = Point{ current.x + end.x, current.y + end.y };
            }
            const bool smooth = previousCommand == 'Q' || previousCommand == 'T';
            const Point c = smooth ? Point{ 2.0f * current.x - lastControl.x,
                                            2.0f * current.y - lastControl.y }
                                   : current;
            Push(commands, PathCommand::Kind::Quad, c, {}, end);
            lastControl = c;
            current = end;
            break;
        }
        case 'A': {
            // 图标里没有圆弧；按直线连到终点，至少不会中断解析。
            float value = 0.0f;
            for (int i = 0; i < 5; ++i) {
                if (!reader.ReadNumber(&value)) {
                    return false;
                }
            }
            float x = 0.0f;
            float y = 0.0f;
            if (!reader.ReadNumber(&x) || !reader.ReadNumber(&y)) {
                return false;
            }
            current = relative ? Point{ current.x + x, current.y + y } : Point{ x, y };
            Push(commands, PathCommand::Kind::Line, current, {}, {});
            break;
        }
        case 'Z':
            Push(commands, PathCommand::Kind::Close, {}, {}, {});
            current = start;
            break;
        default:
            // 不认识的命令：跳过它的字母，继续后面的。
            reader.AdvanceToLetter();
            reader.SkipSeparators();
            break;
        }
        previousCommand = op;
    }
    return !commands->empty();
}

}
