//===--- swift-syntax-test.cpp - Reflection Syntax testing application ----===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This is a host-side tool to perform round-trip testing of "full-fidelity"
// lexing and parsing. That is, when this application ingests a .swift file,
// it should be able to create a list of full tokens, or a full-fidelity AST,
// print them, and get the same file back out. This ensures that we aren't
// losing any source information in these structures.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/LLVMInitialize.h"
#include "swift/Basic/LangOptions.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Frontend/PrintingDiagnosticConsumer.h"
#include "swift/Parse/Lexer.h"
#include "swift/Parse/Parser.h"
#include "swift/Subsystems.h"
#include "swift/Syntax/Serialization/SyntaxDeserialization.h"
#include "swift/Syntax/Serialization/SyntaxSerialization.h"
#include "swift/Syntax/SyntaxData.h"
#include "swift/Syntax/SyntaxNodes.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Regex.h"
#include "llvm/Support/raw_ostream.h"

using namespace swift;
using namespace swift::syntax;
using llvm::StringRef;

enum class ActionType {
  DumpRawTokenSyntax,
  FullLexRoundTrip,
  FullParseRoundTrip,
  SerializeRawTree,
  DeserializeRawTree,
  ParseOnly,
  ParserGen,
  EOFPos,
  None
};

namespace options {
static llvm::cl::OptionCategory Category("swift-syntax-test Options");
static llvm::cl::opt<ActionType>
Action(llvm::cl::desc("Action (required):"),
       llvm::cl::init(ActionType::None),
       llvm::cl::values(
        clEnumValN(ActionType::DumpRawTokenSyntax,
                   "dump-full-tokens",
                   "Lex the source file and dump the tokens "
                   "and their absolute line/column locations"),
        clEnumValN(ActionType::FullLexRoundTrip,
                   "round-trip-lex",
                   "Lex the source file and print it back out for "
                   "comparing against the original"),
        clEnumValN(ActionType::FullParseRoundTrip,
                   "round-trip-parse",
                   "Parse the source file and print it back out for "
                   "comparing against the input"),
        clEnumValN(ActionType::ParseOnly,
                   "parse-only",
                   "Parse the source file with syntax nodes and exit"),
        clEnumValN(ActionType::ParserGen,
                   "parse-gen",
                   "Parse the source file and print it back out for "
                   "comparing against the input"),
        clEnumValN(ActionType::SerializeRawTree,
                   "serialize-raw-tree",
                   "Parse the source file and serialize the raw tree "
                   "to JSON"),
        clEnumValN(ActionType::DeserializeRawTree,
                   "deserialize-raw-tree",
                   "Parse the JSON file from the serialized raw tree "
                   "to the original"),
        clEnumValN(ActionType::EOFPos,
                   "eof",
                   "Parse the source file, calculate the absolute position"
                   "of the EOF token, and dump the buffer from the start of the"
                   "file to the EOF token")));

static llvm::cl::opt<std::string>
InputSourceFilename("input-source-filename",
                    llvm::cl::desc("Path to the input .swift file"));

static llvm::cl::opt<std::string>
InputSourceDirectory("input-source-directory",
                     llvm::cl::desc("Directory to be scanned recursively and "
                                    "run the selected action on every .swift "
                                    "file"));

static llvm::cl::opt<std::string>
OldSyntaxTreeFilename("old-syntax-tree-filename",
                      llvm::cl::desc("Path to the serialized syntax tree of "
                                     "the pre-edit file"));

static llvm::cl::list<std::string>
IncrementalEdits("incremental-edit",
                 llvm::cl::desc("An edit that was applied to reach the input "
                                "file from the source file that generated the "
                                "old syntax tree in the format "
                                "<start>:<end>=<replacement> where <start> and "
                                "<end> are byte offsets in the original file "
                                "and <replacement> is the string that shall "
                                "replace the selected range. "
                                "Can be passed multiple times."));

static llvm::cl::opt<std::string>
IncrementalReuseLog("incremental-reuse-log",
                    llvm::cl::desc("Path to which a log should be written that "
                                   "describes all the nodes reused during "
                                   "incremental parsing."));

static llvm::cl::opt<std::string>
OutputFilename("output-filename",
               llvm::cl::desc("Path to the output file"));

static llvm::cl::opt<bool>
PrintVisualReuseInfo("print-visual-reuse-info",
                     llvm::cl::desc("Print a coloured output of which parts of "
                                    "the source file have been reused from the "
                                    "old syntax tree."),
                     llvm::cl::cat(Category),
                     llvm::cl::init(false));

static llvm::cl::opt<bool>
PrintNodeKind("print-node-kind",
              llvm::cl::desc("To print syntax node kind"),
              llvm::cl::cat(Category),
              llvm::cl::init(false));

static llvm::cl::opt<bool>
PrintTrivialNodeKind("print-trivial-node-kind",
                     llvm::cl::desc("To print trivial syntax node kind"),
                     llvm::cl::cat(Category),
                     llvm::cl::init(false));

static llvm::cl::opt<bool>
VerifySyntaxTree("verify-syntax-tree",
                 llvm::cl::desc("Emit warnings for unknown nodes"),
                 llvm::cl::cat(Category),
                 llvm::cl::init(true));

static llvm::cl::opt<bool>
Visual("v",
       llvm::cl::desc("Print visually"),
       llvm::cl::cat(Category),
       llvm::cl::init(false));
} // end namespace options

namespace {
int getTokensFromFile(unsigned BufferID,
                      LangOptions &LangOpts,
                      SourceManager &SourceMgr,
                      swift::DiagnosticEngine &Diags,
                      std::vector<std::pair<RC<syntax::RawSyntax>,
                      syntax::AbsolutePosition>> &Tokens) {
  Tokens = tokenizeWithTrivia(LangOpts, SourceMgr, BufferID,
                              /*Offset=*/0, /*EndOffset=*/0,
                              &Diags);
  return EXIT_SUCCESS;
}


int
getTokensFromFile(const StringRef InputFilename,
                  LangOptions &LangOpts,
                  SourceManager &SourceMgr,
                  DiagnosticEngine &Diags,
                  std::vector<std::pair<RC<syntax::RawSyntax>,
                                        syntax::AbsolutePosition>> &Tokens) {
  auto Buffer = llvm::MemoryBuffer::getFile(InputFilename);
  if (!Buffer) {
    Diags.diagnose(SourceLoc(), diag::cannot_open_file,
                   InputFilename, Buffer.getError().message());
    return EXIT_FAILURE;
  }

  auto BufferID = SourceMgr.addNewSourceBuffer(std::move(Buffer.get()));
  return getTokensFromFile(BufferID, LangOpts, SourceMgr, Diags, Tokens);
}

void anchorForGetMainExecutable() {}

bool parseIncrementalEditArguments(SyntaxParsingCache *Cache,
                                   SourceManager &SourceMgr,
                                   unsigned BufferID) {
  // Parse the source edits
  for (auto EditPattern : options::IncrementalEdits) {
    llvm::Regex MatchRegex("([0-9]+):([0-9]+)-([0-9]+):([0-9]+)=(.*)");
    SmallVector<StringRef, 4> Matches;
    if (!MatchRegex.match(EditPattern, &Matches)) {
      llvm::errs() << "Invalid edit pattern: " << EditPattern << '\n';
      return false;
    }
    int EditStartLine, EditStartColumn, EditEndLine, EditEndColumn;
    if (Matches[1].getAsInteger(10, EditStartLine)) {
      llvm::errs() << "Could not parse edit start as integer: " << EditStartLine
                   << '\n';
      return false;
    }
    if (Matches[2].getAsInteger(10, EditStartColumn)) {
      llvm::errs() << "Could not parse edit start as integer: "
                   << EditStartColumn << '\n';
      return false;
    }
    if (Matches[3].getAsInteger(10, EditEndLine)) {
      llvm::errs() << "Could not parse edit start as integer: " << EditEndLine
                   << '\n';
      return false;
    }
    if (Matches[4].getAsInteger(10, EditEndColumn)) {
      llvm::errs() << "Could not parse edit end as integer: " << EditEndColumn
                   << '\n';
      return false;
    }

    auto EditStartLoc =
        SourceMgr.getLocForLineCol(BufferID, EditStartLine, EditStartColumn);
    auto EditEndLoc =
        SourceMgr.getLocForLineCol(BufferID, EditEndLine, EditEndColumn);
    auto EditStartOffset =
        SourceMgr.getLocOffsetInBuffer(EditStartLoc, BufferID);
    auto EditEndOffset = SourceMgr.getLocOffsetInBuffer(EditEndLoc, BufferID);
    Cache->addEdit(EditStartOffset, EditEndOffset,
                   /*ReplacmentLength=*/Matches[5].size());
  }
  return true;
}

void printVisualNodeReuseInformation(SourceManager &SourceMgr,
                                     unsigned BufferID,
                                     SyntaxParsingCache *Cache) {
  unsigned CurrentOffset = 0;
  auto SourceText = SourceMgr.getEntireTextForBuffer(BufferID);
  if (llvm::outs().has_colors()) {
    llvm::outs().changeColor(llvm::buffer_ostream::Colors::GREEN);
  }
  auto PrintReparsedRegion = [](StringRef SourceText, unsigned ReparseStart,
                                unsigned ReparseEnd) {
    if (ReparseEnd != ReparseStart) {
      if (llvm::outs().has_colors()) {
        llvm::outs().changeColor(llvm::buffer_ostream::Colors::RED);
      } else {
        llvm::outs() << "<reparse>";
      }

      llvm::outs() << SourceText.substr(ReparseStart,
                                        ReparseEnd - ReparseStart);

      if (llvm::outs().has_colors()) {
        llvm::outs().changeColor(llvm::buffer_ostream::Colors::GREEN);
      } else {
        llvm::outs() << "</reparse>";
      }
    }
  };

  for (auto ReuseRange : Cache->getReusedRanges()) {
    // Print region that was not reused
    PrintReparsedRegion(SourceText, CurrentOffset, ReuseRange.first);

    llvm::outs() << SourceText.substr(ReuseRange.first,
                                      ReuseRange.second - ReuseRange.first);
    CurrentOffset = ReuseRange.second;
  }
  PrintReparsedRegion(SourceText, CurrentOffset, SourceText.size());
  if (llvm::outs().has_colors())
    llvm::outs().resetColor();

  llvm::outs() << '\n';
}

void saveReuseLog(SourceManager &SourceMgr, unsigned BufferID,
                  SyntaxParsingCache *Cache) {
  std::error_code ErrorCode;
  llvm::raw_fd_ostream ReuseLog(options::IncrementalReuseLog, ErrorCode,
                                llvm::sys::fs::OpenFlags::F_RW);
  assert(!ErrorCode && "Unable to open incremental usage log");

  for (auto ReuseRange : Cache->getReusedRanges()) {
    SourceLoc Start = SourceMgr.getLocForOffset(BufferID, ReuseRange.first);
    SourceLoc End = SourceMgr.getLocForOffset(BufferID, ReuseRange.second);

    ReuseLog << "Reused ";
    Start.printLineAndColumn(ReuseLog, SourceMgr, BufferID);
    ReuseLog << " to ";
    End.printLineAndColumn(ReuseLog, SourceMgr, BufferID);
    ReuseLog << '\n';
  }
}

/// Parse the given input file (incrementally if an old syntax tree was
/// provided) and call the action specific callback with the new syntax tree
int parseFile(const char *MainExecutablePath, const StringRef InputFileName,
              llvm::function_ref<int(SourceFile *)> ActionSpecificCallback) {
  // The cache needs to be a heap allocated pointer since we construct it inside
  // an if block but need to keep it alive until the end of the function.
  SyntaxParsingCache *SyntaxCache = nullptr;
  SWIFT_DEFER { delete SyntaxCache; };
  // We also need to hold on to the Deserializer and buffer since they keep
  // ownership of strings that are referenced from the old syntax tree
  swift::json::SyntaxDeserializer *Deserializer = nullptr;
  SWIFT_DEFER { delete Deserializer; };

  auto Buffer = llvm::MemoryBuffer::getFile(options::OldSyntaxTreeFilename);
  // Deserialise the old syntax tree
  if (!options::OldSyntaxTreeFilename.empty()) {
    Deserializer = new swift::json::SyntaxDeserializer(
        llvm::MemoryBufferRef(*(Buffer.get())));
    auto OldSyntaxTree = Deserializer->getSourceFileSyntax();
    if (!OldSyntaxTree.hasValue()) {
      llvm::errs() << "Could not deserialise old syntax tree.";
      return EXIT_FAILURE;
    }
    SyntaxCache = new SyntaxParsingCache(OldSyntaxTree.getValue());

    SyntaxCache->recordReuseInformation();
  }

  // Set up the compiler invocation
  CompilerInvocation Invocation;
  Invocation.getLangOptions().BuildSyntaxTree = true;
  Invocation.getLangOptions().VerifySyntaxTree = options::VerifySyntaxTree;
  Invocation.getFrontendOptions().InputsAndOutputs.addInputFile(InputFileName);
  Invocation.setMainExecutablePath(
    llvm::sys::fs::getMainExecutable(MainExecutablePath,
      reinterpret_cast<void *>(&anchorForGetMainExecutable)));
  Invocation.setMainFileSyntaxParsingCache(SyntaxCache);
  Invocation.setModuleName("Test");

  PrintingDiagnosticConsumer DiagConsumer;
  CompilerInstance Instance;
  Instance.addDiagnosticConsumer(&DiagConsumer);
  if (Instance.setup(Invocation)) {
    llvm::errs() << "Unable to set up compiler instance";
    return EXIT_FAILURE;
  }

  // Parse incremental edit arguments
  auto BufferIDs = Instance.getInputBufferIDs();
  assert(BufferIDs.size() == 1 && "Only expecting to process one source file");
  unsigned BufferID = BufferIDs.front();

  if (SyntaxCache) {
    if (!parseIncrementalEditArguments(SyntaxCache, Instance.getSourceMgr(),
                                       BufferID)) {
      return EXIT_FAILURE;
    }
  }

  // Parse the actual source file
  Instance.performParseOnly();

  SourceFile *SF = nullptr;
  for (auto Unit : Instance.getMainModule()->getFiles()) {
    SF = dyn_cast<SourceFile>(Unit);
    if (SF != nullptr) {
      break;
    }
  }
  assert(SF && "No source file");

  // If we have a syntax cache, output reuse information if requested
  if (SyntaxCache) {
    if (options::PrintVisualReuseInfo) {
      printVisualNodeReuseInformation(Instance.getSourceMgr(), BufferID,
                                      SyntaxCache);
    }
    if (!options::IncrementalReuseLog.empty()) {
      saveReuseLog(Instance.getSourceMgr(), BufferID, SyntaxCache);
    }
  }

  return ActionSpecificCallback(SF);
}

int doFullLexRoundTrip(const StringRef InputFilename) {
  LangOptions LangOpts;
  SourceManager SourceMgr;
  DiagnosticEngine Diags(SourceMgr);
  PrintingDiagnosticConsumer DiagPrinter;
  Diags.addConsumer(DiagPrinter);

  std::vector<std::pair<RC<syntax::RawSyntax>,
                                   syntax::AbsolutePosition>> Tokens;
  if (getTokensFromFile(InputFilename, LangOpts, SourceMgr,
                        Diags, Tokens) == EXIT_FAILURE) {
    return EXIT_FAILURE;
  }

  for (auto TokAndPos : Tokens) {
    TokAndPos.first->print(llvm::outs(), {});
  }

  return EXIT_SUCCESS;
}

int doDumpRawTokenSyntax(const StringRef InputFile) {
  LangOptions LangOpts;
  SourceManager SourceMgr;
  DiagnosticEngine Diags(SourceMgr);
  PrintingDiagnosticConsumer DiagPrinter;
  Diags.addConsumer(DiagPrinter);

  std::vector<std::pair<RC<syntax::RawSyntax>,
                        syntax::AbsolutePosition>> Tokens;
  if (getTokensFromFile(InputFile, LangOpts, SourceMgr, Diags, Tokens) ==
      EXIT_FAILURE) {
    return EXIT_FAILURE;
  }

  for (auto TokAndPos : Tokens) {
    TokAndPos.second.printLineAndColumn(llvm::outs());
    llvm::outs() << "\n";
    TokAndPos.first->dump(llvm::outs());
    llvm::outs() << "\n";
  }

  return EXIT_SUCCESS;
}

int doFullParseRoundTrip(const char *MainExecutablePath,
                         const StringRef InputFile) {
  return parseFile(MainExecutablePath, InputFile, [](SourceFile *SF) -> int {
    SF->getSyntaxRoot().print(llvm::outs(), {});
    return EXIT_SUCCESS;
  });
}

int doSerializeRawTree(const char *MainExecutablePath,
                       const StringRef InputFile) {
  return parseFile(MainExecutablePath, InputFile, [](SourceFile *SF) -> int {
    auto Root = SF->getSyntaxRoot().getRaw();

    if (!options::OutputFilename.empty()) {
      std::error_code errorCode;
      llvm::raw_fd_ostream os(options::OutputFilename, errorCode,
                              llvm::sys::fs::F_None);
      assert(!errorCode && "Couldn't open output file");

      swift::json::Output out(os);
      out << *Root;
      os << "\n";
    } else {
      swift::json::Output out(llvm::outs());
      out << *Root;
      llvm::outs() << "\n";
    }
    return EXIT_SUCCESS;
  });
}

int doDeserializeRawTree(const char *MainExecutablePath,
                         const StringRef InputFile,
                         const StringRef OutputFileName) {

  auto Buffer = llvm::MemoryBuffer::getFile(InputFile);
  std::error_code errorCode;
  auto os = llvm::make_unique<llvm::raw_fd_ostream>(
              OutputFileName, errorCode, llvm::sys::fs::F_None);
  swift::json::SyntaxDeserializer deserializer(llvm::MemoryBufferRef(*(Buffer.get())));
  deserializer.getSourceFileSyntax()->print(*os);

  return EXIT_SUCCESS;
}

int doParseOnly(const char *MainExecutablePath, const StringRef InputFile) {
  return parseFile(MainExecutablePath, InputFile, [](SourceFile *SF) {
    return SF ? EXIT_SUCCESS : EXIT_FAILURE;
  });
}

int dumpParserGen(const char *MainExecutablePath, const StringRef InputFile) {
  return parseFile(MainExecutablePath, InputFile, [](SourceFile *SF) {
    SyntaxPrintOptions Opts;
    Opts.PrintSyntaxKind = options::PrintNodeKind;
    Opts.Visual = options::Visual;
    Opts.PrintTrivialNodeKind = options::PrintTrivialNodeKind;
    SF->getSyntaxRoot().print(llvm::outs(), Opts);
    return EXIT_SUCCESS;
  });
}

int dumpEOFSourceLoc(const char *MainExecutablePath,
                     const StringRef InputFile) {
  return parseFile(MainExecutablePath, InputFile, [](SourceFile *SF) -> int {
    auto BufferId = *SF->getBufferID();
    auto Root = SF->getSyntaxRoot();
    auto AbPos = Root.getEOFToken().getAbsolutePosition();

    SourceManager &SourceMgr = SF->getASTContext().SourceMgr;
    auto StartLoc = SourceMgr.getLocForBufferStart(BufferId);
    auto EndLoc = SourceMgr.getLocForOffset(BufferId, AbPos.getOffset());

    // To ensure the correctness of position when translated to line & column
    // pair.
    if (SourceMgr.getLineAndColumn(EndLoc) != AbPos.getLineAndColumn()) {
      llvm::outs() << "locations should be identical";
      return EXIT_FAILURE;
    }
    llvm::outs() << CharSourceRange(SourceMgr, StartLoc, EndLoc).str();
    return EXIT_SUCCESS;
  });
}
}// end of anonymous namespace

int invokeCommand(const char *MainExecutablePath,
                  const StringRef InputSourceFilename) {
  int ExitCode = EXIT_SUCCESS;
  
  switch (options::Action) {
    case ActionType::DumpRawTokenSyntax:
      ExitCode = doDumpRawTokenSyntax(InputSourceFilename);
      break;
    case ActionType::FullLexRoundTrip:
      ExitCode = doFullLexRoundTrip(InputSourceFilename);
      break;
    case ActionType::FullParseRoundTrip:
      ExitCode = doFullParseRoundTrip(MainExecutablePath, InputSourceFilename);
      break;
    case ActionType::SerializeRawTree:
      ExitCode = doSerializeRawTree(MainExecutablePath, InputSourceFilename);
      break;
    case ActionType::DeserializeRawTree:
      ExitCode = doDeserializeRawTree(MainExecutablePath, InputSourceFilename,
                                      options::OutputFilename);
      break;
    case ActionType::ParseOnly:
      ExitCode = doParseOnly(MainExecutablePath, InputSourceFilename);
      break;
    case ActionType::ParserGen:
      ExitCode = dumpParserGen(MainExecutablePath, InputSourceFilename);
      break;
    case ActionType::EOFPos:
      ExitCode = dumpEOFSourceLoc(MainExecutablePath, InputSourceFilename);
      break;
    case ActionType::None:
      llvm::errs() << "an action is required\n";
      llvm::cl::PrintHelpMessage();
      ExitCode = EXIT_FAILURE;
      break;
  }
  
  return ExitCode;
}

int main(int argc, char *argv[]) {
  PROGRAM_START(argc, argv);
  llvm::cl::ParseCommandLineOptions(argc, argv, "Swift Syntax Test\n");

  int ExitCode = EXIT_SUCCESS;

  if (options::InputSourceFilename.empty() &&
      options::InputSourceDirectory.empty()) {
    llvm::errs() << "input source file is required\n";
    ExitCode = EXIT_FAILURE;
  }
  
  if (!options::InputSourceFilename.empty() &&
      !options::InputSourceDirectory.empty()) {
    llvm::errs() << "input-source-filename and input-source-directory cannot "
                    "be used together\n\n";
    ExitCode = EXIT_FAILURE;
  }
  
  if (options::Action == ActionType::None) {
    llvm::errs() << "an action is required\n";
    ExitCode = EXIT_FAILURE;
  }

  if (ExitCode == EXIT_FAILURE) {
    llvm::cl::PrintHelpMessage();
    return ExitCode;
  }

  if (!options::InputSourceFilename.empty()) {
    ExitCode = invokeCommand(argv[0], options::InputSourceFilename);
  } else {
    assert(!options::InputSourceDirectory.empty());
    std::error_code errorCode;
    llvm::sys::fs::recursive_directory_iterator DI(options::InputSourceDirectory, errorCode);
    llvm::sys::fs::recursive_directory_iterator endIterator;
    for (; DI != endIterator; DI.increment(errorCode)) {
      auto entry = *DI;
      auto path = entry.path();
      if (!llvm::sys::fs::is_directory(path) &&
          StringRef(path).endswith(".swift")) {
        ExitCode = invokeCommand(argv[0], path);
      }
    }
  }

  return ExitCode;
}
