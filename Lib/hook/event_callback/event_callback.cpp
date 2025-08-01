#include "chatgpt_api.h"
#include "event_callback.h"
#include "post_event.h"
#include <ApplicationServices/ApplicationServices.h>
#include <iostream>
#include <string>
#include <sstream>
#include <filesystem>
#include <mach-o/dyld.h>
#include <limits.h>
#include <CoreFoundation/CoreFoundation.h>

// Declare postUnicodeChar if not declared in post_event.h
void postUnicodeChar(char16_t c) {
    UniChar chars[1] = { static_cast<UniChar>(c) };
    CGEventRef keyDown = CGEventCreateKeyboardEvent(NULL, 0, true);
    CGEventRef keyUp = CGEventCreateKeyboardEvent(NULL, 0, false);
    CGEventKeyboardSetUnicodeString(keyDown, 1, chars);
    CGEventKeyboardSetUnicodeString(keyUp, 1, chars);
    CGEventPost(kCGHIDEventTap, keyDown);
    CGEventPost(kCGHIDEventTap, keyUp);
    CFRelease(keyDown);
    CFRelease(keyUp);
}
// Return 키 keycodeㅎ
static constexpr CGKeyCode KEY_RETURN    = 36;
// Backspace keycode
static constexpr CGKeyCode KEY_BACKSPACE = 51;

// 사용자가 엔터 전까지 입력한 Unicode 문자를 저장할 버퍼
static std::u16string inputBuffer;

// utf16 <-> utf8 변환 함수 추가
static std::string utf16_to_utf8(const std::u16string& s) {
    if (s.empty()) return "";
    CFStringRef cfString = CFStringCreateWithCharacters(kCFAllocatorDefault,
                                                        reinterpret_cast<const UniChar*>(s.data()),
                                                        s.length());
    if (!cfString) return "";

    CFIndex bufferSize = CFStringGetMaximumSizeForEncoding(CFStringGetLength(cfString), kCFStringEncodingUTF8) + 1;
    char buffer[bufferSize];
    
    std::string result;
    if (CFStringGetCString(cfString, buffer, bufferSize, kCFStringEncodingUTF8)) {
        result = buffer;
    }

    CFRelease(cfString);
    return result;
}
static std::u16string utf8_to_utf16(const std::string& s) {
    if (s.empty()) return u"";
    CFStringRef cfString = CFStringCreateWithCString(kCFAllocatorDefault,
                                                     s.c_str(),
                                                     kCFStringEncodingUTF8);
    if (!cfString) return u"";

    CFIndex length = CFStringGetLength(cfString);
    std::u16string result(length, 0);
    CFStringGetCharacters(cfString, CFRangeMake(0, length), reinterpret_cast<UniChar*>(result.data()));

    CFRelease(cfString);
    return result;
}

// 실행 파일의 절대 경로를 기반으로 로고 파일의 경로를 반환합니다.
static std::string getLogoPath() {
    char path_buf[PATH_MAX];
    uint32_t size = sizeof(path_buf);
    if (_NSGetExecutablePath(path_buf, &size) != 0) {
        // 경로를 가져오는 데 실패했습니다.
        return "";
    }
    // 실행 파일 경로: /path/to/project/Output/keyhook
    std::filesystem::path execPath(path_buf);
    // 프로젝트 루트 경로: /path/to/project
    std::filesystem::path projectRoot = execPath.parent_path().parent_path();
    std::filesystem::path logoPath = projectRoot / "img" / "logo.png";

    // 파일이 실제로 존재하는지 확인합니다.
    if (std::filesystem::exists(logoPath)) {
        return logoPath.string();
    }
    return "";
}

// macOS 대화상자를 띄워서 Yes/No 를 리턴
static bool askUserConfirm() {
    const char* script =
        "osascript -e 'display dialog \"ChatGPT를 호출하시겠습니까?\" "
                     "buttons {\"예\",\"아니요\"} default button \"예\"'";
    FILE* pipe = popen(script, "r");
    if (!pipe) return false;
    std::string result;
    char buf[128];
    while (fgets(buf, sizeof(buf), pipe)) {
        result += buf;
    }
    pclose(pipe);
    return result.find("예") != std::string::npos;
}

CGEventRef eventCallback(CGEventTapProxy proxy,
                         CGEventType   type,
                         CGEventRef    event,
                         void*         refcon)
{
    // 탭이 비활성화되었을 때 재활성화
    if (type == kCGEventTapDisabledByTimeout ||
        type == kCGEventTapDisabledByUserInput)
    {
        extern CFMachPortRef gEventTap;
        CGEventTapEnable(gEventTap, true);
        return event;
    }

    // 키 다운 이벤트만 처리
    if (type == kCGEventKeyDown) {
        int64_t keycode = CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);

        // 1) Return(엔터) 키 처리
        if (keycode == KEY_RETURN) {
            // 버퍼가 비어있으면, 사용자는 일반적인 엔터 입력을 의도한 것이므로
            // 아무런 동작 없이 이벤트를 통과시킵니다.
            // 문장 교체 직후의 엔터 입력이 여기에 해당됩니다.
            if (inputBuffer.empty()) {
                return event;
            }
            
            if (askUserConfirm()) { // "ChatGPT를 호출하시겠습니까?"
                const auto originalLength = inputBuffer.length();
                std::string prompt = utf16_to_utf8(inputBuffer);
                std::string reply = ask_chatgpt(prompt, "입력한 문장을 자연스럽게 다듬어, 쉼표로 구분된 3가지 버전(첫번째, 두번째, 세번째)만 제안하세요. 각 문장만 쉼표로 구분해서 출력하고, 설명이나 안내, 추가 멘트는 절대 포함하지 마세요.");

                std::vector<std::string> sentences;
                std::istringstream ss(reply);
                std::string part;
                while (std::getline(ss, part, ',')) {
                    if (!part.empty()) sentences.push_back(part);
                    if (sentences.size() == 3) break;
                }
                while (sentences.size() < 3) sentences.push_back("");

                std::string logoPath = getLogoPath();
                std::string iconScript;
                if (!logoPath.empty()) {
                    iconScript = " with icon POSIX file \"" + logoPath + "\"";
                }

                // 1. 문장 선택창
                std::string osaCmd = "osascript -e 'display dialog \"아래 문장 중 하나를 선택하세요\"" +
                    iconScript +
                    " buttons {\"" + sentences[2] + "\",\"" + sentences[1] + "\",\"" + sentences[0] + "\"} default button \"" + sentences[0] + "\"'";
                FILE* pipe = popen(osaCmd.c_str(), "r");
                std::string osaResult;
                char buf[256];
                while (fgets(buf, sizeof(buf), pipe)) {
                    osaResult += buf;
                }
                pclose(pipe);

                std::string chosen;
                for (const auto& s : sentences) {
                    if (!s.empty() && osaResult.find(s) != std::string::npos) {
                        chosen = s;
                        break;
                    }
                }

                if (!chosen.empty()) {
                    // 2. 최종 교체 확인창
                    std::string confirmScriptStr = "osascript -e 'display dialog \"입력한 내용을 바꾸시겠습니까?\"" +
                                                   iconScript + 
                                                   " buttons {\"예\",\"아니요\"} default button \"예\"'";
                    FILE* confirmPipe = popen(confirmScriptStr.c_str(), "r");
                    std::string confirmResult;
                    char confirmBuf[128];
                    while (fgets(confirmBuf, sizeof(confirmBuf), confirmPipe)) {
                        confirmResult += confirmBuf;
                    }
                    pclose(confirmPipe);

                    if (confirmResult.find("예") != std::string::npos) {
                        // Delete previously typed input with backspace
                        for (size_t i = 0; i < originalLength; ++i) {
                            CGEventRef backspaceDown = CGEventCreateKeyboardEvent(NULL, KEY_BACKSPACE, true);
                            CGEventRef backspaceUp = CGEventCreateKeyboardEvent(NULL, KEY_BACKSPACE, false);
                            CGEventPost(kCGHIDEventTap, backspaceDown);
                            CGEventPost(kCGHIDEventTap, backspaceUp);
                            CFRelease(backspaceDown);
                            CFRelease(backspaceUp);
                        }

                        std::u16string chosen16 = utf8_to_utf16(chosen);
                        for (auto c : chosen16) {
                            postUnicodeChar(c);
                        }
                        // 교체 작업이 완료되었으므로 버퍼를 비워서
                        // 다음 엔터 키 입력이 일반 시스템 입력으로 처리되도록 합니다.
                        inputBuffer.clear();
                    }
                }

                return NULL; // Prevent original input from being passed to system
            }

            // 사용자가 ChatGPT 호출을 원치 않으면 버퍼를 비우고 엔터키를 통과시킴
            inputBuffer.clear();
            return event; // pass through Enter if not replacing
        }

        // 2) 엔터가 아니면 Unicode 문자 읽어서 버퍼에 추가
        UniChar uniBuf[1];
        UniCharCount actualLen = 0;
        CGEventKeyboardGetUnicodeString(event, 1, &actualLen, uniBuf);
        if (actualLen == 1) {
            inputBuffer.push_back(uniBuf[0]);
        }
    }

    // 기타 이벤트는 원본 그대로 전달
    return event;
}
