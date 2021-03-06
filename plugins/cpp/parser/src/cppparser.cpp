#include <algorithm>
#include <unordered_map>
#include <memory>
#include <thread>

#include <boost/algorithm/string/join.hpp>
#include <boost/filesystem.hpp>

#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>

#include <model/buildaction.h>
#include <model/buildaction-odb.hxx>
#include <model/buildsourcetarget.h>
#include <model/buildsourcetarget-odb.hxx>
#include <model/file.h>
#include <model/file-odb.hxx>

#include <util/hash.h>
#include <util/odbtransaction.h>
#include <util/logutil.h>

#include <cppparser/cppparser.h>

#include "assignmentcollector.h"
#include "clangastvisitor.h"

namespace cc
{
namespace parser
{

class VisitorActionFactory : public clang::tooling::FrontendActionFactory
{
public:
  static void cleanUp()
  {
    MyConsumer::_mangledNameCache.clear();
  }

  VisitorActionFactory(ParserContext& ctx_) : _ctx(ctx_)
  {
  }

  clang::FrontendAction* create() override
  {
    return new MyFrontendAction(_ctx);
  }

private:
  class MyConsumer : public clang::ASTConsumer
  {
    friend class VisitorActionFactory;

  public:
    MyConsumer(ParserContext& ctx_, clang::ASTContext& context_)
      : _ctx(ctx_), _context(context_)
    {
    }

    virtual void HandleTranslationUnit(clang::ASTContext& context_) override
    {
      {
        ClangASTVisitor clangAstVisitor(
          _ctx, _context, _mangledNameCache, _clangToAstNodeId);
        clangAstVisitor.TraverseDecl(context_.getTranslationUnitDecl());
      }

      {
        AssignmentCollector assignmentCollector(
          _ctx, _context, _mangledNameCache, _clangToAstNodeId);
        assignmentCollector.TraverseDecl(context_.getTranslationUnitDecl());
      }
    }

  private:
    static std::unordered_map<model::CppAstNodeId, std::uint64_t>
      _mangledNameCache;
    std::unordered_map<const void*, model::CppAstNodeId>
      _clangToAstNodeId;

    ParserContext& _ctx;
    clang::ASTContext& _context;
  };

  class MyFrontendAction : public clang::ASTFrontendAction
  {
  public:
    MyFrontendAction(ParserContext& ctx_) : _ctx(ctx_)
    {
    }

    virtual std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
      clang::CompilerInstance& compiler_, llvm::StringRef) override
    {
      return std::unique_ptr<clang::ASTConsumer>(
        new MyConsumer(_ctx, compiler_.getASTContext()));
    }

  private:
    ParserContext& _ctx;
  };

  ParserContext& _ctx;
};

std::unordered_map<model::CppAstNodeId, std::uint64_t>
VisitorActionFactory::MyConsumer::_mangledNameCache;

bool CppParser::isSourceFile(const std::string& file_) const
{
  const std::vector<std::string> cppExts{
    ".c", ".cc", ".cpp", ".cxx", ".o", ".so", ".a"};

  std::string ext = boost::filesystem::extension(file_);
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

  return std::find(cppExts.begin(), cppExts.end(), ext) != cppExts.end();
}

std::map<std::string, std::string> CppParser::extractInputOutputs(
  const clang::tooling::CompileCommand& command_) const
{
  std::map<std::string, std::string> inToOut;

  enum State
  {
    None,
    OParam
  };

  bool hasCParam = false;
  std::unordered_set<std::string> sources;
  std::string output;

  State state = None;
  for (const std::string& arg : command_.CommandLine)
  {
    if (state == OParam)
    {
      output = arg;
      state = None;
    }
    else if (isSourceFile(arg))
      sources.insert(arg);
    else if (arg == "-c")
      hasCParam = true;
    else if (arg == "-o")
      state = OParam;
  }

  if (output.empty() && hasCParam)
  {
    for (const std::string& src : sources)
    {
      std::string extension = boost::filesystem::extension(src);
      inToOut[src] = src.substr(0, src.size() - extension.size() - 1) + ".o";
    }
  }
  else
  {
    if (output.empty())
      output = command_.Directory + "/a.out";

    for (const std::string& src : sources)
      inToOut[src] = output;
  }

  return inToOut;
}

void CppParser::addCompileCommand(
  const clang::tooling::CompileCommand& command_)
{
  util::OdbTransaction transaction(_ctx.db);

  //--- BuildAction ---//

  model::BuildActionPtr buildAction(new model::BuildAction);

  std::string extension = boost::filesystem::extension(command_.Filename);

  buildAction->command = boost::algorithm::join(command_.CommandLine, " ");
  buildAction->type
    = extension == ".o" || extension == ".so" || extension == ".a"
    ? model::BuildAction::Link
    : model::BuildAction::Compile;

  transaction([&, this]{ _ctx.db->persist(buildAction); });

  //--- BuildSource, BuildTarget ---//

  std::vector<model::BuildSource> sources;
  std::vector<model::BuildTarget> targets;

  for (const auto& srcTarget : extractInputOutputs(command_))
  {
    if (!boost::filesystem::exists(srcTarget.first) ||
        !boost::filesystem::exists(srcTarget.second))
      continue;

    model::BuildSource buildSource;
    buildSource.file = _ctx.srcMgr.getFile(srcTarget.first);
    buildSource.action = buildAction;
    sources.push_back(std::move(buildSource));

    model::BuildTarget buildTarget;
    buildTarget.file = _ctx.srcMgr.getFile(srcTarget.second);
    buildTarget.action = buildAction;
    targets.push_back(std::move(buildTarget));
  }

  _ctx.srcMgr.persistFiles();

  transaction([&, this] {
    for (model::BuildSource buildSource : sources)
      _ctx.db->persist(buildSource);
    for (model::BuildTarget buildTarget : targets)
      _ctx.db->persist(buildTarget);
  });
}

void CppParser::worker()
{
  static std::mutex mutex;

  while (true)
  {
    //--- Select nect compilation command ---//

    mutex.lock();

    if (_index == _compileCommands.size())
    {
      mutex.unlock();
      break;
    }

    const clang::tooling::CompileCommand& command = _compileCommands[_index];
    std::size_t index = ++_index;

    //--- Add compile command hash ---//

    auto hash = util::fnvHash(
      boost::algorithm::join(command.CommandLine, ""));

    if (_parsedCommandHashes.find(hash) != _parsedCommandHashes.end())
    {
      LOG(info)
        << '(' << index << '/' << _compileCommands.size() << ')'
        << " Already parsed " << command.Filename;
      mutex.unlock();
      continue;
    }

    _parsedCommandHashes.insert(hash);

    mutex.unlock();

    //--- Assemble compiler command line ---//

    std::vector<const char*> commandLine;
    commandLine.reserve(command.CommandLine.size());
    commandLine.push_back("--");
    std::transform(
      command.CommandLine.begin() + 1, // Skip compiler name
      command.CommandLine.end(),
      std::back_inserter(commandLine),
      [](const std::string& s){ return s.c_str(); });

    int argc = commandLine.size();

    std::unique_ptr<clang::tooling::FixedCompilationDatabase> compilationDb(
      clang::tooling::FixedCompilationDatabase::loadFromCommandLine(
        argc,
        commandLine.data()));

    //--- Start the tool ---//

    VisitorActionFactory factory(_ctx);

    LOG(info)
      << '(' << index << '/' << _compileCommands.size() << ')'
      << " Parsing " << command.Filename;

    clang::tooling::ClangTool tool(*compilationDb, command.Filename);

    tool.run(&factory);

    //--- Save build command ---//

    addCompileCommand(command);
  }
}

CppParser::CppParser(ParserContext& ctx_) : AbstractParser(ctx_)
{
  (util::OdbTransaction(_ctx.db))([&, this] {
    for (const model::BuildAction& ba : _ctx.db->query<model::BuildAction>())
      _parsedCommandHashes.insert(util::fnvHash(ba.command));
  });
}
  
std::vector<std::string> CppParser::getDependentParsers() const
{
  return std::vector<std::string>{};
}

bool CppParser::parse()
{
  bool success = true;

  for (const std::string& input
    : _ctx.options["input"].as<std::vector<std::string>>())
    if (boost::filesystem::is_regular_file(input))
      success
        = success && parseByJson(input, _ctx.options["jobs"].as<int>());

  VisitorActionFactory::cleanUp();
  _parsedCommandHashes.clear();

  return success;
}

bool CppParser::parseByJson(
  const std::string& jsonFile_,
  std::size_t threadNum_)
{
  std::string errorMsg;

  std::unique_ptr<clang::tooling::JSONCompilationDatabase> compDb
    = clang::tooling::JSONCompilationDatabase::loadFromFile(
        jsonFile_, errorMsg);

  if (!errorMsg.empty())
  {
    LOG(error) << errorMsg;
    return false;
  }

  _compileCommands = compDb->getAllCompileCommands();
  _index = 0;

  std::vector<std::thread> workers;
  workers.reserve(threadNum_);

  for (std::size_t i = 0; i < threadNum_; ++i)
    workers.emplace_back(&cc::parser::CppParser::worker, this);

  for (std::thread& worker : workers)
    worker.join();

  return true;
}

CppParser::~CppParser()
{
}

extern "C"
{
  boost::program_options::options_description getOptions()
  {
    boost::program_options::options_description description("C++ Plugin");
    return description;
  }

  std::shared_ptr<CppParser> make(ParserContext& ctx_)
  {
    return std::make_shared<CppParser>(ctx_);
  }
}

} // parser
} // cc
