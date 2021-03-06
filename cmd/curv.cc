// Copyright Doug Moen 2016-2017.
// Distributed under The MIT License.
// See accompanying file LICENCE.md or https://opensource.org/licenses/MIT

extern "C" {
#include "readlinex.h"
#include <string.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
}
#include <iostream>
#include <fstream>

#include "progdir.h"
#include <aux/dtostr.h>
#include <curv/analyzer.h>
#include <curv/context.h>
#include <curv/program.h>
#include <curv/exception.h>
#include <curv/file.h>
#include <curv/parser.h>
#include <curv/phrase.h>
#include <curv/shared.h>
#include <curv/system.h>
#include <curv/list.h>
#include <curv/record.h>
#include <curv/gl_compiler.h>
#include <curv/shape.h>

bool was_interrupted = false;

void interrupt_handler(int)
{
    was_interrupted = true;
}

struct CString_Script : public curv::Script
{
    char* buffer_;

    // buffer argument is a static string.
    CString_Script(const char* name, const char* buffer)
    :
        curv::Script(curv::make_string(name), buffer, buffer + strlen(buffer)),
        buffer_(nullptr)
    {
    }

    // buffer argument is a heap string, allocated using malloc.
    CString_Script(const char* name, char* buffer)
    :
        curv::Script(curv::make_string(name), buffer, buffer + strlen(buffer)),
        buffer_(buffer)
    {}

    ~CString_Script()
    {
        if (buffer_) free(buffer_);
    }
};

curv::System&
make_system(const char* argv0, std::list<const char*>& libs)
{
    try {
        static curv::System_Impl sys(std::cerr);
        if (argv0 != nullptr) {
            const char* CURV_STDLIB = getenv("CURV_STDLIB");
            namespace fs = boost::filesystem;
            curv::Shared<const curv::String> stdlib;
            if (CURV_STDLIB != nullptr) {
                if (CURV_STDLIB[0] != '\0')
                    stdlib = curv::make_string(CURV_STDLIB);
                else
                    stdlib = nullptr;
            } else {
                fs::path stdlib_path = progdir(argv0) / "../lib/std.curv";
                stdlib = curv::make_string(stdlib_path.c_str());
            }
            sys.load_library(stdlib);
        }
        for (const char* lib : libs) {
            sys.load_library(curv::make_string(lib));
        }
        return sys;
    } catch (curv::Exception& e) {
        std::cerr << "ERROR: " << e << "\n";
        exit(EXIT_FAILURE);
    } catch (std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        exit(EXIT_FAILURE);
    }
}

curv::Shared<curv::String>
make_tempfile()
{
    auto filename = curv::stringify(",curv",getpid(),".frag");
    int fd = creat(filename->c_str(), 0666);
    if (fd == -1)
        throw curv::Exception({}, curv::stringify(
            "Can't create ",filename->c_str(),": ",strerror(errno)));
    close(fd);
    return filename;
}

void
display_shape(curv::Value value, const curv::Context &cx, bool block = false)
{
    static bool viewer = false;
    curv::Shape_Recognizer shape;
    if (shape.recognize(value, cx)) {
        auto filename = make_tempfile();
        std::ofstream f(filename->c_str());
        curv::gl_compile(shape, f, cx);
        f.close();
        if (!viewer) {
            auto cmd = curv::stringify("glslViewer ",filename->c_str(),
                block ? "" : "&");
            system(cmd->c_str());
            if (block) unlink(filename->c_str());
            viewer = true;
        }
    }
}

int
interactive_mode(curv::System& sys)
{
    // Catch keyboard interrupts, and set was_interrupted = true.
    // This is/will be used to interrupt the evaluator.
    struct sigaction interrupt_action;
    memset((void*)&interrupt_action, 0, sizeof(interrupt_action));
    interrupt_action.sa_handler = interrupt_handler;
    sigaction(SIGINT, &interrupt_action, nullptr);

    // top level definitions, extended by typing 'id = expr'
    curv::Namespace names = sys.std_namespace();

    for (;;) {
        // Race condition on assignment to was_interrupted.
        was_interrupted = false;
        RLXResult result;
        char* line = readlinex("curv> ", &result);
        if (line == nullptr) {
            std::cout << "\n";
            if (result == rlx_interrupt) {
                continue;
            }
            return EXIT_SUCCESS;
        }
        auto script = curv::make<CString_Script>("", line);
        try {
            curv::Program prog{*script, sys};
            prog.compile(&names, nullptr);
            auto den = prog.denotes();
            if (den.first) {
                for (auto f : *den.first)
                    names[f.first] = curv::make<curv::Builtin_Value>(f.second);
            }
            if (den.second) {
                for (auto e : *den.second)
                    std::cout << e << "\n";
                if (den.second->size() == 1)
                    display_shape(den.second->front(),
                        curv::At_Phrase(prog.value_phrase(), nullptr));
            }
        } catch (curv::Exception& e) {
            std::cout << "ERROR: " << e << "\n";
        } catch (std::exception& e) {
            std::cout << "ERROR: " << e.what() << "\n";
        }
    }
}

void export_curv(curv::Value value, const curv::Context&, std::ostream& out)
{
    out << value << "\n";
}
void export_frag(curv::Value value, const curv::Context& cx, std::ostream& out)
{
    curv::Shape_Recognizer shape;
    if (shape.recognize(value, cx))
        curv::gl_compile(shape, std::cout, cx);
    else
        throw curv::Exception(cx, "not a shape");
}
bool is_json_data(curv::Value val)
{
    if (val.is_ref()) {
        auto& ref = val.get_ref_unsafe();
        switch (ref.type_) {
        case curv::Ref_Value::ty_string:
        case curv::Ref_Value::ty_list:
        case curv::Ref_Value::ty_record:
            return true;
        default:
            return false;
        }
    } else {
        return true; // null, bool or num
    }
}
bool export_json_value(curv::Value val, std::ostream& out)
{
    if (val.is_null()) {
        out << "null";
        return true;
    }
    if (val.is_bool()) {
        out << val;
        return true;
    }
    if (val.is_num()) {
        out << aux::dfmt(val.get_num_unsafe(), aux::dfmt::JSON);
        return true;
    }
    assert(val.is_ref());
    auto& ref = val.get_ref_unsafe();
    switch (ref.type_) {
    case curv::Ref_Value::ty_string:
      {
        auto& str = (curv::String&)ref;
        out << '"';
        for (auto c : str) {
            if (c == '\\' || c == '"')
                out << '\\';
            out << c;
        }
        out << '"';
        return true;
      }
    case curv::Ref_Value::ty_list:
      {
        auto& list = (curv::List&)ref;
        out << "[";
        bool first = true;
        for (auto e : list) {
            if (is_json_data(e)) {
                if (!first) out << ",";
                first = false;
                export_json_value(e, out);
            }
        }
        out << "]";
        return true;
      }
    case curv::Ref_Value::ty_record:
      {
        auto& record = (curv::Record&)ref;
        out << "{";
        bool first = true;
        for (auto i : record.fields_) {
            if (is_json_data(i.second)) {
                if (!first) out << ",";
                first = false;
                out << '"' << i.first << "\":";
                export_json_value(i.second, out);
            }
        }
        out << "}";
        return true;
      }
    default:
        return false;
    }
}
void export_json(curv::Value value, const curv::Context& cx, std::ostream& out)
{
    if (export_json_value(value, out))
        out << "\n";
    else
        throw curv::Exception(cx, "value can't be converted to JSON");
}
void export_png(curv::Value value, const curv::Context& cx, std::ostream& out)
{
    curv::Shape_Recognizer shape;
    if (shape.recognize(value, cx)) {
        auto fragname = curv::stringify(",curv",getpid(),".frag");
        auto pngname = curv::stringify(",curv",getpid(),".png");
        std::ofstream f(fragname->c_str());
        curv::gl_compile(shape, f, cx);
        f.close();
        auto cmd = curv::stringify(
            "glslViewer -s 0 --headless -o ", pngname->c_str(),
            " ", fragname->c_str(), " >/dev/null");
        system(cmd->c_str());
        auto cmd2 = curv::stringify("cat ",pngname->c_str());
        system(cmd2->c_str());
        unlink(fragname->c_str());
        unlink(pngname->c_str());
    } else
        throw curv::Exception(cx, "not a shape");
}

int
live_mode(curv::System& sys, const char* editor, const char* filename)
{
    if (editor != nullptr) {
        auto cmd = curv::stringify(editor, " ", filename);
        system(cmd->c_str());
    }
    for (;;) {
        struct stat st;
        if (stat(filename, &st) != 0) {
            // file doesn't exist.
            memset((void*)&st, 0, sizeof(st));
        } else {
            // evaluate file.
            try {
                auto file = curv::make<curv::File_Script>(
                    curv::make_string(filename), curv::Context{});
                curv::Program prog{*file, sys};
                prog.compile();
                auto value = prog.eval();
                std::cout << value << "\n";
                display_shape(value,
                    curv::At_Phrase(prog.value_phrase(), nullptr));
            } catch (curv::Exception& e) {
                std::cout << "ERROR: " << e << "\n";
            } catch (std::exception& e) {
                std::cout << "ERROR: " << e.what() << "\n";
            }
        }
        // Wait for file to change.
        for (;;) {
            usleep(500'000);
            struct stat st2;
            if (stat(filename, &st2) != 0)
                memset((void*)&st2, 0, sizeof(st));
            if (st.st_mtime != st2.st_mtime)
                break;
        }
    }
}

const char help[] =
"curv [options] [filename]\n"
"-n -- don't use standard library\n"
"-u file -- use specified library; may be repeated\n"
"-l -- live programming mode\n"
"-e -- run <$CURV_EDITOR filename> in live mode\n"
"-x -- interpret filename argument as expression\n"
"-o format -- output format:\n"
"   curv -- Curv expression\n"
"   json -- JSON expression\n"
"   frag -- GLSL fragment shader (shape only, shadertoy.com compatible)\n"
"   png -- PNG image file (shape only)\n"
"--version -- display version.\n"
"--help -- display this help information.\n"
"filename -- input file, a Curv script. Interactive CLI if missing.\n"
;

int
main(int argc, char** argv)
{
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        std::cout << help;
        return EXIT_SUCCESS;
    }
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        std::cout << "Curv: initial development phase (no version yet)\n";
        return EXIT_SUCCESS;
    }

    // Parse arguments.
    const char* argv0 = argv[0];
    void (*exporter)(curv::Value, const curv::Context&, std::ostream&) =
        nullptr;
    bool live = false;
    std::list<const char*> libs;
    bool expr = false;
    const char* editor = nullptr;

    int opt;
    while ((opt = getopt(argc, argv, ":o:lnu:xe")) != -1) {
        switch (opt) {
        case 'o':
            if (strcmp(optarg, "curv") == 0)
                exporter = export_curv;
            else if (strcmp(optarg, "json") == 0)
                exporter = export_json;
            else if (strcmp(optarg, "frag") == 0)
                exporter = export_frag;
            else if (strcmp(optarg, "png") == 0)
                exporter = export_png;
            else {
                std::cerr << "-o: format " << optarg << " not supported\n"
                          << "Use " << argv0 << " --help for help.\n";
                return EXIT_FAILURE;
            }
            break;
        case 'l':
            live = true;
            break;
        case 'n':
            argv0 = nullptr;
            break;
        case 'u':
            libs.push_back(optarg);
            break;
        case 'x':
            expr = true;
            break;
        case 'e':
            editor = getenv("CURV_EDITOR");
            if (editor == nullptr) {
                std::cerr << "-e specified but $CURV_EDITOR not defined\n"
                         << "Use " << argv0 << " --help for help.\n";
                return EXIT_FAILURE;
            }
            break;
        case '?':
            std::cerr << "-" << (char)optopt << ": unknown option\n"
                     << "Use " << argv0 << " --help for help.\n";
            return EXIT_FAILURE;
        case ':':
            std::cerr << "-" << (char)optopt << ": missing argument\n"
                     << "Use " << argv0 << " --help for help.\n";
            return EXIT_FAILURE;
        default:
            assert(0);
        }
    }
    const char* filename;
    if (optind >= argc) {
        filename = nullptr;
    } else if (argc - optind > 1) {
        std::cerr << "too many filename arguments\n"
                  << "Use " << argv0 << " --help for help.\n";
        return EXIT_FAILURE;
    } else
        filename = argv[optind];

    // Validate arguments
    if (live) {
        if (exporter) {
            std::cerr << "-l and -o flags are not compatible.\n"
                      << "Use " << argv0 << " --help for help.\n";
            return EXIT_FAILURE;
        }
    }
    if (filename == nullptr) {
        if (expr) {
            std::cerr << "missing expression argument\n"
                      << "Use " << argv0 << " --help for help.\n";
            return EXIT_FAILURE;
        }
        if (exporter != nullptr || live) {
            std::cerr << "missing filename argument\n"
                      << "Use " << argv0 << " --help for help.\n";
            return EXIT_FAILURE;
        }
    }
    if (editor && !live) {
        std::cerr << "-e flag specified without -l flag.\n"
                  << "Use " << argv0 << " --help for help.\n";
        return EXIT_FAILURE;
    }

    // Interpret arguments
    curv::System& sys(make_system(argv0, libs));

    if (filename == nullptr) {
        return interactive_mode(sys);
    }

    if (live) {
        return live_mode(sys, editor, filename);
    }

    // batch mode
    try {
        curv::Shared<curv::Script> script;
        if (expr) {
            script = curv::make<CString_Script>("", filename);
        } else {
            script = curv::make<curv::File_Script>(
                curv::make_string(filename), curv::Context{});
        }

        curv::Program prog{*script, sys};
        prog.compile();
        auto value = prog.eval();

        if (exporter == nullptr) {
            std::cout << value << "\n";
            display_shape(value,
                curv::At_Phrase(prog.value_phrase(), nullptr),
                true);
        } else {
            exporter(value,
                curv::At_Phrase(prog.value_phrase(), nullptr),
                std::cout);
        }
    } catch (curv::Exception& e) {
        std::cerr << "ERROR: " << e << "\n";
        return EXIT_FAILURE;
    } catch (std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
