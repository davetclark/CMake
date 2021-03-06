/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file Copyright.txt or https://cmake.org/licensing for details.  */
#include "cmServerProtocol.h"

#include "cmAlgorithms.h"
#include "cmExternalMakefileProjectGenerator.h"
#include "cmFileMonitor.h"
#include "cmGeneratorExpression.h"
#include "cmGeneratorTarget.h"
#include "cmGlobalGenerator.h"
#include "cmInstallGenerator.h"
#include "cmInstallTargetGenerator.h"
#include "cmLinkLineComputer.h"
#include "cmLocalGenerator.h"
#include "cmMakefile.h"
#include "cmProperty.h"
#include "cmServer.h"
#include "cmServerDictionary.h"
#include "cmSourceFile.h"
#include "cmState.h"
#include "cmStateDirectory.h"
#include "cmStateSnapshot.h"
#include "cmStateTypes.h"
#include "cmSystemTools.h"
#include "cmTarget.h"
#include "cmTest.h"
#include "cm_uv.h"
#include "cmake.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

// Get rid of some windows macros:
#undef max

namespace {

std::vector<std::string> getConfigurations(const cmake* cm)
{
  std::vector<std::string> configurations;
  auto makefiles = cm->GetGlobalGenerator()->GetMakefiles();
  if (makefiles.empty()) {
    return configurations;
  }

  makefiles[0]->GetConfigurations(configurations);
  if (configurations.empty()) {
    configurations.push_back("");
  }
  return configurations;
}

bool hasString(const Json::Value& v, const std::string& s)
{
  return !v.isNull() &&
    std::any_of(v.begin(), v.end(),
                [s](const Json::Value& i) { return i.asString() == s; });
}

template <class T>
Json::Value fromStringList(const T& in)
{
  Json::Value result = Json::arrayValue;
  for (std::string const& i : in) {
    result.append(i);
  }
  return result;
}

std::vector<std::string> toStringList(const Json::Value& in)
{
  std::vector<std::string> result;
  for (auto const& it : in) {
    result.push_back(it.asString());
  }
  return result;
}

void getCMakeInputs(const cmGlobalGenerator* gg, const std::string& sourceDir,
                    const std::string& buildDir,
                    std::vector<std::string>* internalFiles,
                    std::vector<std::string>* explicitFiles,
                    std::vector<std::string>* tmpFiles)
{
  const std::string cmakeRootDir = cmSystemTools::GetCMakeRoot() + '/';
  std::vector<cmMakefile*> const& makefiles = gg->GetMakefiles();
  for (cmMakefile const* mf : makefiles) {
    for (std::string const& lf : mf->GetListFiles()) {

      const std::string startOfFile = lf.substr(0, cmakeRootDir.size());
      const bool isInternal = (startOfFile == cmakeRootDir);
      const bool isTemporary = !isInternal && (lf.find(buildDir + '/') == 0);

      std::string toAdd = lf;
      if (!sourceDir.empty()) {
        const std::string& relative =
          cmSystemTools::RelativePath(sourceDir, lf);
        if (toAdd.size() > relative.size()) {
          toAdd = relative;
        }
      }

      if (isInternal) {
        if (internalFiles) {
          internalFiles->push_back(std::move(toAdd));
        }
      } else {
        if (isTemporary) {
          if (tmpFiles) {
            tmpFiles->push_back(std::move(toAdd));
          }
        } else {
          if (explicitFiles) {
            explicitFiles->push_back(std::move(toAdd));
          }
        }
      }
    }
  }
}

} // namespace

cmServerRequest::cmServerRequest(cmServer* server, cmConnection* connection,
                                 const std::string& t, const std::string& c,
                                 const Json::Value& d)
  : Type(t)
  , Cookie(c)
  , Data(d)
  , Connection(connection)
  , m_Server(server)
{
}

void cmServerRequest::ReportProgress(int min, int current, int max,
                                     const std::string& message) const
{
  this->m_Server->WriteProgress(*this, min, current, max, message);
}

void cmServerRequest::ReportMessage(const std::string& message,
                                    const std::string& title) const
{
  m_Server->WriteMessage(*this, message, title);
}

cmServerResponse cmServerRequest::Reply(const Json::Value& data) const
{
  cmServerResponse response(*this);
  response.SetData(data);
  return response;
}

cmServerResponse cmServerRequest::ReportError(const std::string& message) const
{
  cmServerResponse response(*this);
  response.SetError(message);
  return response;
}

cmServerResponse::cmServerResponse(const cmServerRequest& request)
  : Type(request.Type)
  , Cookie(request.Cookie)
{
}

void cmServerResponse::SetData(const Json::Value& data)
{
  assert(this->m_Payload == PAYLOAD_UNKNOWN);
  if (!data[kCOOKIE_KEY].isNull() || !data[kTYPE_KEY].isNull()) {
    this->SetError("Response contains cookie or type field.");
    return;
  }
  this->m_Payload = PAYLOAD_DATA;
  this->m_Data = data;
}

void cmServerResponse::SetError(const std::string& message)
{
  assert(this->m_Payload == PAYLOAD_UNKNOWN);
  this->m_Payload = PAYLOAD_ERROR;
  this->m_ErrorMessage = message;
}

bool cmServerResponse::IsComplete() const
{
  return this->m_Payload != PAYLOAD_UNKNOWN;
}

bool cmServerResponse::IsError() const
{
  assert(this->m_Payload != PAYLOAD_UNKNOWN);
  return this->m_Payload == PAYLOAD_ERROR;
}

std::string cmServerResponse::ErrorMessage() const
{
  if (this->m_Payload == PAYLOAD_ERROR) {
    return this->m_ErrorMessage;
  }
  return std::string();
}

Json::Value cmServerResponse::Data() const
{
  assert(this->m_Payload != PAYLOAD_UNKNOWN);
  return this->m_Data;
}

bool cmServerProtocol::Activate(cmServer* server,
                                const cmServerRequest& request,
                                std::string* errorMessage)
{
  assert(server);
  this->m_Server = server;
  this->m_CMakeInstance = cm::make_unique<cmake>(cmake::RoleProject);
  const bool result = this->DoActivate(request, errorMessage);
  if (!result) {
    this->m_CMakeInstance = nullptr;
  }
  return result;
}

cmFileMonitor* cmServerProtocol::FileMonitor() const
{
  return this->m_Server ? this->m_Server->FileMonitor() : nullptr;
}

void cmServerProtocol::SendSignal(const std::string& name,
                                  const Json::Value& data) const
{
  if (this->m_Server) {
    this->m_Server->WriteSignal(name, data);
  }
}

cmake* cmServerProtocol::CMakeInstance() const
{
  return this->m_CMakeInstance.get();
}

bool cmServerProtocol::DoActivate(const cmServerRequest& /*request*/,
                                  std::string* /*errorMessage*/)
{
  return true;
}

std::pair<int, int> cmServerProtocol1::ProtocolVersion() const
{
  return std::make_pair(1, 2);
}

static void setErrorMessage(std::string* errorMessage, const std::string& text)
{
  if (errorMessage) {
    *errorMessage = text;
  }
}

static bool getOrTestHomeDirectory(cmState* state, std::string& value,
                                   std::string* errorMessage)
{
  const std::string cachedValue =
    std::string(state->GetCacheEntryValue("CMAKE_HOME_DIRECTORY"));
  if (value.empty()) {
    value = cachedValue;
    return true;
  }
  const std::string suffix = "/CMakeLists.txt";
  const std::string cachedValueCML = cachedValue + suffix;
  const std::string valueCML = value + suffix;
  if (!cmSystemTools::SameFile(valueCML, cachedValueCML)) {
    setErrorMessage(errorMessage,
                    std::string("\"CMAKE_HOME_DIRECTORY\" is set but "
                                "incompatible with configured "
                                "source directory value."));
    return false;
  }
  return true;
}

static bool getOrTestValue(cmState* state, const std::string& key,
                           std::string& value,
                           const std::string& keyDescription,
                           std::string* errorMessage)
{
  const char* entry = state->GetCacheEntryValue(key);
  const std::string cachedValue =
    entry == nullptr ? std::string() : std::string(entry);
  if (value.empty()) {
    value = cachedValue;
  }
  if (!cachedValue.empty() && cachedValue != value) {
    setErrorMessage(errorMessage, std::string("\"") + key +
                      "\" is set but incompatible with configured " +
                      keyDescription + " value.");
    return false;
  }
  return true;
}

bool cmServerProtocol1::DoActivate(const cmServerRequest& request,
                                   std::string* errorMessage)
{
  std::string sourceDirectory = request.Data[kSOURCE_DIRECTORY_KEY].asString();
  const std::string buildDirectory =
    request.Data[kBUILD_DIRECTORY_KEY].asString();
  std::string generator = request.Data[kGENERATOR_KEY].asString();
  std::string extraGenerator = request.Data[kEXTRA_GENERATOR_KEY].asString();
  std::string toolset = request.Data[kTOOLSET_KEY].asString();
  std::string platform = request.Data[kPLATFORM_KEY].asString();

  if (buildDirectory.empty()) {
    setErrorMessage(errorMessage, std::string("\"") + kBUILD_DIRECTORY_KEY +
                      "\" is missing.");
    return false;
  }

  cmake* cm = CMakeInstance();
  if (cmSystemTools::PathExists(buildDirectory)) {
    if (!cmSystemTools::FileIsDirectory(buildDirectory)) {
      setErrorMessage(errorMessage, std::string("\"") + kBUILD_DIRECTORY_KEY +
                        "\" exists but is not a directory.");
      return false;
    }

    const std::string cachePath = cm->FindCacheFile(buildDirectory);
    if (cm->LoadCache(cachePath)) {
      cmState* state = cm->GetState();

      // Check generator:
      if (!getOrTestValue(state, "CMAKE_GENERATOR", generator, "generator",
                          errorMessage)) {
        return false;
      }

      // check extra generator:
      if (!getOrTestValue(state, "CMAKE_EXTRA_GENERATOR", extraGenerator,
                          "extra generator", errorMessage)) {
        return false;
      }

      // check sourcedir:
      if (!getOrTestHomeDirectory(state, sourceDirectory, errorMessage)) {
        return false;
      }

      // check toolset:
      if (!getOrTestValue(state, "CMAKE_GENERATOR_TOOLSET", toolset, "toolset",
                          errorMessage)) {
        return false;
      }

      // check platform:
      if (!getOrTestValue(state, "CMAKE_GENERATOR_PLATFORM", platform,
                          "platform", errorMessage)) {
        return false;
      }
    }
  }

  if (sourceDirectory.empty()) {
    setErrorMessage(errorMessage, std::string("\"") + kSOURCE_DIRECTORY_KEY +
                      "\" is unset but required.");
    return false;
  }
  if (!cmSystemTools::FileIsDirectory(sourceDirectory)) {
    setErrorMessage(errorMessage, std::string("\"") + kSOURCE_DIRECTORY_KEY +
                      "\" is not a directory.");
    return false;
  }
  if (generator.empty()) {
    setErrorMessage(errorMessage, std::string("\"") + kGENERATOR_KEY +
                      "\" is unset but required.");
    return false;
  }

  std::vector<cmake::GeneratorInfo> generators;
  cm->GetRegisteredGenerators(generators);
  auto baseIt = std::find_if(generators.begin(), generators.end(),
                             [&generator](const cmake::GeneratorInfo& info) {
                               return info.name == generator;
                             });
  if (baseIt == generators.end()) {
    setErrorMessage(errorMessage, std::string("Generator \"") + generator +
                      "\" not supported.");
    return false;
  }
  auto extraIt = std::find_if(
    generators.begin(), generators.end(),
    [&generator, &extraGenerator](const cmake::GeneratorInfo& info) {
      return info.baseName == generator && info.extraName == extraGenerator;
    });
  if (extraIt == generators.end()) {
    setErrorMessage(errorMessage,
                    std::string("The combination of generator \"" + generator +
                                "\" and extra generator \"" + extraGenerator +
                                "\" is not supported."));
    return false;
  }
  if (!extraIt->supportsToolset && !toolset.empty()) {
    setErrorMessage(errorMessage,
                    std::string("Toolset was provided but is not supported by "
                                "the requested generator."));
    return false;
  }
  if (!extraIt->supportsPlatform && !platform.empty()) {
    setErrorMessage(errorMessage,
                    std::string("Platform was provided but is not supported "
                                "by the requested generator."));
    return false;
  }

  this->GeneratorInfo =
    GeneratorInformation(generator, extraGenerator, toolset, platform,
                         sourceDirectory, buildDirectory);

  this->m_State = STATE_ACTIVE;
  return true;
}

void cmServerProtocol1::HandleCMakeFileChanges(const std::string& path,
                                               int event, int status)
{
  assert(status == 0);
  static_cast<void>(status);

  if (!m_isDirty) {
    m_isDirty = true;
    SendSignal(kDIRTY_SIGNAL, Json::objectValue);
  }
  Json::Value obj = Json::objectValue;
  obj[kPATH_KEY] = path;
  Json::Value properties = Json::arrayValue;
  if (event & UV_RENAME) {
    properties.append(kRENAME_PROPERTY_VALUE);
  }
  if (event & UV_CHANGE) {
    properties.append(kCHANGE_PROPERTY_VALUE);
  }

  obj[kPROPERTIES_KEY] = properties;
  SendSignal(kFILE_CHANGE_SIGNAL, obj);
}

const cmServerResponse cmServerProtocol1::Process(
  const cmServerRequest& request)
{
  assert(this->m_State >= STATE_ACTIVE);

  if (request.Type == kCACHE_TYPE) {
    return this->ProcessCache(request);
  }
  if (request.Type == kCMAKE_INPUTS_TYPE) {
    return this->ProcessCMakeInputs(request);
  }
  if (request.Type == kCODE_MODEL_TYPE) {
    return this->ProcessCodeModel(request);
  }
  if (request.Type == kCOMPUTE_TYPE) {
    return this->ProcessCompute(request);
  }
  if (request.Type == kCONFIGURE_TYPE) {
    return this->ProcessConfigure(request);
  }
  if (request.Type == kFILESYSTEM_WATCHERS_TYPE) {
    return this->ProcessFileSystemWatchers(request);
  }
  if (request.Type == kGLOBAL_SETTINGS_TYPE) {
    return this->ProcessGlobalSettings(request);
  }
  if (request.Type == kSET_GLOBAL_SETTINGS_TYPE) {
    return this->ProcessSetGlobalSettings(request);
  }
  if (request.Type == kCTEST_INFO_TYPE) {
    return this->ProcessCTests(request);
  }

  return request.ReportError("Unknown command!");
}

bool cmServerProtocol1::IsExperimental() const
{
  return true;
}

cmServerResponse cmServerProtocol1::ProcessCache(
  const cmServerRequest& request)
{
  cmState* state = this->CMakeInstance()->GetState();

  Json::Value result = Json::objectValue;

  std::vector<std::string> allKeys = state->GetCacheEntryKeys();

  Json::Value list = Json::arrayValue;
  std::vector<std::string> keys = toStringList(request.Data[kKEYS_KEY]);
  if (keys.empty()) {
    keys = allKeys;
  } else {
    for (auto const& i : keys) {
      if (std::find(allKeys.begin(), allKeys.end(), i) == allKeys.end()) {
        return request.ReportError("Key \"" + i + "\" not found in cache.");
      }
    }
  }
  std::sort(keys.begin(), keys.end());
  for (auto const& key : keys) {
    Json::Value entry = Json::objectValue;
    entry[kKEY_KEY] = key;
    entry[kTYPE_KEY] =
      cmState::CacheEntryTypeToString(state->GetCacheEntryType(key));
    entry[kVALUE_KEY] = state->GetCacheEntryValue(key);

    Json::Value props = Json::objectValue;
    bool haveProperties = false;
    for (auto const& prop : state->GetCacheEntryPropertyList(key)) {
      haveProperties = true;
      props[prop] = state->GetCacheEntryProperty(key, prop);
    }
    if (haveProperties) {
      entry[kPROPERTIES_KEY] = props;
    }

    list.append(entry);
  }

  result[kCACHE_KEY] = list;
  return request.Reply(result);
}

cmServerResponse cmServerProtocol1::ProcessCMakeInputs(
  const cmServerRequest& request)
{
  if (this->m_State < STATE_CONFIGURED) {
    return request.ReportError("This instance was not yet configured.");
  }

  const cmake* cm = this->CMakeInstance();
  const cmGlobalGenerator* gg = cm->GetGlobalGenerator();
  const std::string cmakeRootDir = cmSystemTools::GetCMakeRoot();
  const std::string& buildDir = cm->GetHomeOutputDirectory();
  const std::string& sourceDir = cm->GetHomeDirectory();

  Json::Value result = Json::objectValue;
  result[kSOURCE_DIRECTORY_KEY] = sourceDir;
  result[kCMAKE_ROOT_DIRECTORY_KEY] = cmakeRootDir;

  std::vector<std::string> internalFiles;
  std::vector<std::string> explicitFiles;
  std::vector<std::string> tmpFiles;
  getCMakeInputs(gg, sourceDir, buildDir, &internalFiles, &explicitFiles,
                 &tmpFiles);

  Json::Value array = Json::arrayValue;

  Json::Value tmp = Json::objectValue;
  tmp[kIS_CMAKE_KEY] = true;
  tmp[kIS_TEMPORARY_KEY] = false;
  tmp[kSOURCES_KEY] = fromStringList(internalFiles);
  array.append(tmp);

  tmp = Json::objectValue;
  tmp[kIS_CMAKE_KEY] = false;
  tmp[kIS_TEMPORARY_KEY] = false;
  tmp[kSOURCES_KEY] = fromStringList(explicitFiles);
  array.append(tmp);

  tmp = Json::objectValue;
  tmp[kIS_CMAKE_KEY] = false;
  tmp[kIS_TEMPORARY_KEY] = true;
  tmp[kSOURCES_KEY] = fromStringList(tmpFiles);
  array.append(tmp);

  result[kBUILD_FILES_KEY] = array;

  return request.Reply(result);
}

class LanguageData
{
public:
  bool operator==(const LanguageData& other) const;

  void SetDefines(const std::set<std::string>& defines);

  bool IsGenerated = false;
  std::string Language;
  std::string Flags;
  std::vector<std::string> Defines;
  std::vector<std::pair<std::string, bool>> IncludePathList;
};

bool LanguageData::operator==(const LanguageData& other) const
{
  return Language == other.Language && Defines == other.Defines &&
    Flags == other.Flags && IncludePathList == other.IncludePathList &&
    IsGenerated == other.IsGenerated;
}

void LanguageData::SetDefines(const std::set<std::string>& defines)
{
  std::vector<std::string> result;
  result.reserve(defines.size());
  for (std::string const& i : defines) {
    result.push_back(i);
  }
  std::sort(result.begin(), result.end());
  Defines = std::move(result);
}

namespace std {

template <>
struct hash<LanguageData>
{
  std::size_t operator()(const LanguageData& in) const
  {
    using std::hash;
    size_t result =
      hash<std::string>()(in.Language) ^ hash<std::string>()(in.Flags);
    for (auto const& i : in.IncludePathList) {
      result = result ^ (hash<std::string>()(i.first) ^
                         (i.second ? std::numeric_limits<size_t>::max() : 0));
    }
    for (auto const& i : in.Defines) {
      result = result ^ hash<std::string>()(i);
    }
    result =
      result ^ (in.IsGenerated ? std::numeric_limits<size_t>::max() : 0);
    return result;
  }
};

} // namespace std

static Json::Value DumpSourceFileGroup(const LanguageData& data,
                                       const std::vector<std::string>& files,
                                       const std::string& baseDir)
{
  Json::Value result = Json::objectValue;

  if (!data.Language.empty()) {
    result[kLANGUAGE_KEY] = data.Language;
    if (!data.Flags.empty()) {
      result[kCOMPILE_FLAGS_KEY] = data.Flags;
    }
    if (!data.IncludePathList.empty()) {
      Json::Value includes = Json::arrayValue;
      for (auto const& i : data.IncludePathList) {
        Json::Value tmp = Json::objectValue;
        tmp[kPATH_KEY] = i.first;
        if (i.second) {
          tmp[kIS_SYSTEM_KEY] = i.second;
        }
        includes.append(tmp);
      }
      result[kINCLUDE_PATH_KEY] = includes;
    }
    if (!data.Defines.empty()) {
      result[kDEFINES_KEY] = fromStringList(data.Defines);
    }
  }

  result[kIS_GENERATED_KEY] = data.IsGenerated;

  Json::Value sourcesValue = Json::arrayValue;
  for (auto const& i : files) {
    const std::string relPath = cmSystemTools::RelativePath(baseDir, i);
    sourcesValue.append(relPath.size() < i.size() ? relPath : i);
  }

  result[kSOURCES_KEY] = sourcesValue;
  return result;
}

static Json::Value DumpSourceFilesList(
  cmGeneratorTarget* target, const std::string& config,
  const std::map<std::string, LanguageData>& languageDataMap)
{
  // Collect sourcefile groups:

  std::vector<cmSourceFile*> files;
  target->GetSourceFiles(files, config);

  std::unordered_map<LanguageData, std::vector<std::string>> fileGroups;
  for (cmSourceFile* file : files) {
    LanguageData fileData;
    fileData.Language = file->GetLanguage();
    if (!fileData.Language.empty()) {
      const LanguageData& ld = languageDataMap.at(fileData.Language);
      cmLocalGenerator* lg = target->GetLocalGenerator();
      cmGeneratorExpressionInterpreter genexInterpreter(
        lg, target, config, target->GetName(), fileData.Language);

      std::string compileFlags = ld.Flags;
      const std::string COMPILE_FLAGS("COMPILE_FLAGS");
      if (const char* cflags = file->GetProperty(COMPILE_FLAGS)) {
        lg->AppendFlags(compileFlags,
                        genexInterpreter.Evaluate(cflags, COMPILE_FLAGS));
      }
      const std::string COMPILE_OPTIONS("COMPILE_OPTIONS");
      if (const char* coptions = file->GetProperty(COMPILE_OPTIONS)) {
        lg->AppendCompileOptions(
          compileFlags, genexInterpreter.Evaluate(coptions, COMPILE_OPTIONS));
      }
      fileData.Flags = compileFlags;

      // Add include directories from source file properties.
      std::vector<std::string> includes;

      const std::string INCLUDE_DIRECTORIES("INCLUDE_DIRECTORIES");
      if (const char* cincludes = file->GetProperty(INCLUDE_DIRECTORIES)) {
        const char* evaluatedIncludes =
          genexInterpreter.Evaluate(cincludes, INCLUDE_DIRECTORIES);
        lg->AppendIncludeDirectories(includes, evaluatedIncludes, *file);

        for (const auto& include : includes) {
          fileData.IncludePathList.push_back(std::make_pair(
            include, target->IsSystemIncludeDirectory(include, config)));
        }
      }

      fileData.IncludePathList.insert(fileData.IncludePathList.end(),
                                      ld.IncludePathList.begin(),
                                      ld.IncludePathList.end());

      const std::string COMPILE_DEFINITIONS("COMPILE_DEFINITIONS");
      std::set<std::string> defines;
      if (const char* defs = file->GetProperty(COMPILE_DEFINITIONS)) {
        lg->AppendDefines(
          defines, genexInterpreter.Evaluate(defs, COMPILE_DEFINITIONS));
      }

      const std::string defPropName =
        "COMPILE_DEFINITIONS_" + cmSystemTools::UpperCase(config);
      if (const char* config_defs = file->GetProperty(defPropName)) {
        lg->AppendDefines(defines, genexInterpreter.Evaluate(
                                     config_defs, COMPILE_DEFINITIONS));
      }

      defines.insert(ld.Defines.begin(), ld.Defines.end());

      fileData.SetDefines(defines);
    }

    fileData.IsGenerated = file->GetPropertyAsBool("GENERATED");
    std::vector<std::string>& groupFileList = fileGroups[fileData];
    groupFileList.push_back(file->GetFullPath());
  }

  const std::string baseDir = target->Makefile->GetCurrentSourceDirectory();
  Json::Value result = Json::arrayValue;
  for (auto const& it : fileGroups) {
    Json::Value group = DumpSourceFileGroup(it.first, it.second, baseDir);
    if (!group.isNull()) {
      result.append(group);
    }
  }

  return result;
}

static Json::Value DumpCTestInfo(cmLocalGenerator* lg, cmTest* testInfo,
                                 const std::string& config)
{
  Json::Value result = Json::objectValue;
  result[kCTEST_NAME] = testInfo->GetName();

  // Concat command entries together. After the first should be the arguments
  // for the command
  std::string command;
  for (auto const& cmd : testInfo->GetCommand()) {
    command.append(cmd);
    command.append(" ");
  }

  // Remove any config specific variables from the output.
  cmGeneratorExpression ge;
  auto cge = ge.Parse(command.c_str());
  const char* processed = cge->Evaluate(lg, config);

  result[kCTEST_COMMAND] = processed;

  // Build up the list of properties that may have been specified
  Json::Value properties = Json::arrayValue;
  for (auto& prop : testInfo->GetProperties()) {
    Json::Value entry = Json::objectValue;
    entry[kKEY_KEY] = prop.first;

    // Remove config variables from the value too.
    auto cge_value = ge.Parse(prop.second.GetValue());
    const char* processed_value = cge_value->Evaluate(lg, config);
    entry[kVALUE_KEY] = processed_value;
    properties.append(entry);
  }
  result[kPROPERTIES_KEY] = properties;

  return result;
}

static void DumpMakefileTests(cmLocalGenerator* lg, const std::string& config,
                              Json::Value* result)
{
  auto mf = lg->GetMakefile();
  std::vector<cmTest*> tests;
  mf->GetTests(config, tests);
  for (auto test : tests) {
    Json::Value tmp = DumpCTestInfo(lg, test, config);
    if (!tmp.isNull()) {
      result->append(tmp);
    }
  }
}

static Json::Value DumpCTestProjectList(const cmake* cm,
                                        std::string const& config)
{
  Json::Value result = Json::arrayValue;

  auto globalGen = cm->GetGlobalGenerator();

  for (const auto& projectIt : globalGen->GetProjectMap()) {
    Json::Value pObj = Json::objectValue;
    pObj[kNAME_KEY] = projectIt.first;

    Json::Value tests = Json::arrayValue;

    // Gather tests for every generator
    for (const auto& lg : projectIt.second) {
      // Make sure they're generated.
      lg->GenerateTestFiles();
      DumpMakefileTests(lg, config, &tests);
    }

    pObj[kCTEST_INFO] = tests;

    result.append(pObj);
  }

  return result;
}

static Json::Value DumpCTestConfiguration(const cmake* cm,
                                          const std::string& config)
{
  Json::Value result = Json::objectValue;
  result[kNAME_KEY] = config;

  result[kPROJECTS_KEY] = DumpCTestProjectList(cm, config);

  return result;
}

static Json::Value DumpCTestConfigurationsList(const cmake* cm)
{
  Json::Value result = Json::arrayValue;

  for (const std::string& c : getConfigurations(cm)) {
    result.append(DumpCTestConfiguration(cm, c));
  }

  return result;
}

static Json::Value DumpTarget(cmGeneratorTarget* target,
                              const std::string& config)
{
  cmLocalGenerator* lg = target->GetLocalGenerator();
  const cmState* state = lg->GetState();

  const cmStateEnums::TargetType type = target->GetType();
  const std::string typeName = state->GetTargetTypeName(type);

  Json::Value ttl = Json::arrayValue;
  ttl.append("EXECUTABLE");
  ttl.append("STATIC_LIBRARY");
  ttl.append("SHARED_LIBRARY");
  ttl.append("MODULE_LIBRARY");
  ttl.append("OBJECT_LIBRARY");
  ttl.append("UTILITY");
  ttl.append("INTERFACE_LIBRARY");

  if (!hasString(ttl, typeName) || target->IsImported()) {
    return Json::Value();
  }

  Json::Value result = Json::objectValue;
  result[kNAME_KEY] = target->GetName();
  result[kIS_GENERATOR_PROVIDED_KEY] =
    target->Target->GetIsGeneratorProvided();
  result[kTYPE_KEY] = typeName;
  result[kSOURCE_DIRECTORY_KEY] = lg->GetCurrentSourceDirectory();
  result[kBUILD_DIRECTORY_KEY] = lg->GetCurrentBinaryDirectory();

  if (type == cmStateEnums::INTERFACE_LIBRARY) {
    return result;
  }

  result[kFULL_NAME_KEY] = target->GetFullName(config);

  if (target->Target->GetHaveInstallRule()) {
    result[kHAS_INSTALL_RULE] = true;

    Json::Value installPaths = Json::arrayValue;
    auto targetGenerators = target->Makefile->GetInstallGenerators();
    for (auto installGenerator : targetGenerators) {
      auto installTargetGenerator =
        dynamic_cast<cmInstallTargetGenerator*>(installGenerator);
      if (installTargetGenerator != nullptr &&
          installTargetGenerator->GetTarget()->Target == target->Target) {
        auto dest = installTargetGenerator->GetDestination(config);

        std::string installPath;
        if (!dest.empty() && cmSystemTools::FileIsFullPath(dest)) {
          installPath = dest;
        } else {
          std::string installPrefix =
            target->Makefile->GetSafeDefinition("CMAKE_INSTALL_PREFIX");
          installPath = installPrefix + '/' + dest;
        }

        installPaths.append(installPath);
      }
    }

    result[kINSTALL_PATHS] = installPaths;
  }

  if (target->HaveWellDefinedOutputFiles()) {
    Json::Value artifacts = Json::arrayValue;
    artifacts.append(
      target->GetFullPath(config, cmStateEnums::RuntimeBinaryArtifact));
    if (target->IsDLLPlatform()) {
      artifacts.append(
        target->GetFullPath(config, cmStateEnums::ImportLibraryArtifact));
      const cmGeneratorTarget::OutputInfo* output =
        target->GetOutputInfo(config);
      if (output && !output->PdbDir.empty()) {
        artifacts.append(output->PdbDir + '/' + target->GetPDBName(config));
      }
    }
    result[kARTIFACTS_KEY] = artifacts;

    result[kLINKER_LANGUAGE_KEY] = target->GetLinkerLanguage(config);

    std::string linkLibs;
    std::string linkFlags;
    std::string linkLanguageFlags;
    std::string frameworkPath;
    std::string linkPath;
    cmLinkLineComputer linkLineComputer(lg,
                                        lg->GetStateSnapshot().GetDirectory());
    lg->GetTargetFlags(&linkLineComputer, config, linkLibs, linkLanguageFlags,
                       linkFlags, frameworkPath, linkPath, target);

    linkLibs = cmSystemTools::TrimWhitespace(linkLibs);
    linkFlags = cmSystemTools::TrimWhitespace(linkFlags);
    linkLanguageFlags = cmSystemTools::TrimWhitespace(linkLanguageFlags);
    frameworkPath = cmSystemTools::TrimWhitespace(frameworkPath);
    linkPath = cmSystemTools::TrimWhitespace(linkPath);

    if (!cmSystemTools::TrimWhitespace(linkLibs).empty()) {
      result[kLINK_LIBRARIES_KEY] = linkLibs;
    }
    if (!cmSystemTools::TrimWhitespace(linkFlags).empty()) {
      result[kLINK_FLAGS_KEY] = linkFlags;
    }
    if (!cmSystemTools::TrimWhitespace(linkLanguageFlags).empty()) {
      result[kLINK_LANGUAGE_FLAGS_KEY] = linkLanguageFlags;
    }
    if (!frameworkPath.empty()) {
      result[kFRAMEWORK_PATH_KEY] = frameworkPath;
    }
    if (!linkPath.empty()) {
      result[kLINK_PATH_KEY] = linkPath;
    }
    const std::string sysroot =
      lg->GetMakefile()->GetSafeDefinition("CMAKE_SYSROOT");
    if (!sysroot.empty()) {
      result[kSYSROOT_KEY] = sysroot;
    }
  }

  std::set<std::string> languages;
  target->GetLanguages(languages, config);
  std::map<std::string, LanguageData> languageDataMap;
  for (std::string const& lang : languages) {
    LanguageData& ld = languageDataMap[lang];
    ld.Language = lang;
    lg->GetTargetCompileFlags(target, config, lang, ld.Flags);
    std::set<std::string> defines;
    lg->GetTargetDefines(target, config, lang, defines);
    ld.SetDefines(defines);
    std::vector<std::string> includePathList;
    lg->GetIncludeDirectories(includePathList, target, lang, config, true);
    for (std::string const& i : includePathList) {
      ld.IncludePathList.push_back(
        std::make_pair(i, target->IsSystemIncludeDirectory(i, config)));
    }
  }

  Json::Value sourceGroupsValue =
    DumpSourceFilesList(target, config, languageDataMap);
  if (!sourceGroupsValue.empty()) {
    result[kFILE_GROUPS_KEY] = sourceGroupsValue;
  }

  return result;
}

static Json::Value DumpTargetsList(
  const std::vector<cmLocalGenerator*>& generators, const std::string& config)
{
  Json::Value result = Json::arrayValue;

  std::vector<cmGeneratorTarget*> targetList;
  for (auto const& lgIt : generators) {
    const auto& list = lgIt->GetGeneratorTargets();
    targetList.insert(targetList.end(), list.begin(), list.end());
  }
  std::sort(targetList.begin(), targetList.end());

  for (cmGeneratorTarget* target : targetList) {
    Json::Value tmp = DumpTarget(target, config);
    if (!tmp.isNull()) {
      result.append(tmp);
    }
  }

  return result;
}

static Json::Value DumpProjectList(const cmake* cm, std::string const& config)
{
  Json::Value result = Json::arrayValue;

  auto globalGen = cm->GetGlobalGenerator();

  for (auto const& projectIt : globalGen->GetProjectMap()) {
    Json::Value pObj = Json::objectValue;
    pObj[kNAME_KEY] = projectIt.first;

    // All Projects must have at least one local generator
    assert(!projectIt.second.empty());
    const cmLocalGenerator* lg = projectIt.second.at(0);

    // Project structure information:
    const cmMakefile* mf = lg->GetMakefile();
    auto minVersion = mf->GetDefinition("CMAKE_MINIMUM_REQUIRED_VERSION");
    pObj[kMINIMUM_CMAKE_VERSION] = minVersion ? minVersion : "";
    pObj[kSOURCE_DIRECTORY_KEY] = mf->GetCurrentSourceDirectory();
    pObj[kBUILD_DIRECTORY_KEY] = mf->GetCurrentBinaryDirectory();
    pObj[kTARGETS_KEY] = DumpTargetsList(projectIt.second, config);

    // For a project-level install rule it might be defined in any of its
    // associated generators.
    bool hasInstallRule = false;
    for (const auto generator : projectIt.second) {
      hasInstallRule =
        generator->GetMakefile()->GetInstallGenerators().empty() == false;

      if (hasInstallRule) {
        break;
      }
    }

    pObj[kHAS_INSTALL_RULE] = hasInstallRule;

    result.append(pObj);
  }

  return result;
}

static Json::Value DumpConfiguration(const cmake* cm,
                                     const std::string& config)
{
  Json::Value result = Json::objectValue;
  result[kNAME_KEY] = config;

  result[kPROJECTS_KEY] = DumpProjectList(cm, config);

  return result;
}

static Json::Value DumpConfigurationsList(const cmake* cm)
{
  Json::Value result = Json::arrayValue;

  for (std::string const& c : getConfigurations(cm)) {
    result.append(DumpConfiguration(cm, c));
  }

  return result;
}

cmServerResponse cmServerProtocol1::ProcessCodeModel(
  const cmServerRequest& request)
{
  if (this->m_State != STATE_COMPUTED) {
    return request.ReportError("No build system was generated yet.");
  }

  Json::Value result = Json::objectValue;
  result[kCONFIGURATIONS_KEY] = DumpConfigurationsList(this->CMakeInstance());
  return request.Reply(result);
}

cmServerResponse cmServerProtocol1::ProcessCompute(
  const cmServerRequest& request)
{
  if (this->m_State > STATE_CONFIGURED) {
    return request.ReportError("This build system was already generated.");
  }
  if (this->m_State < STATE_CONFIGURED) {
    return request.ReportError("This project was not configured yet.");
  }

  cmake* cm = this->CMakeInstance();
  int ret = cm->Generate();

  if (ret < 0) {
    return request.ReportError("Failed to compute build system.");
  }
  m_State = STATE_COMPUTED;
  return request.Reply(Json::Value());
}

cmServerResponse cmServerProtocol1::ProcessConfigure(
  const cmServerRequest& request)
{
  if (this->m_State == STATE_INACTIVE) {
    return request.ReportError("This instance is inactive.");
  }

  FileMonitor()->StopMonitoring();

  std::string errorMessage;
  cmake* cm = this->CMakeInstance();
  this->GeneratorInfo.SetupGenerator(cm, &errorMessage);
  if (!errorMessage.empty()) {
    return request.ReportError(errorMessage);
  }

  // Make sure the types of cacheArguments matches (if given):
  std::vector<std::string> cacheArgs = { "unused" };
  bool cacheArgumentsError = false;
  const Json::Value passedArgs = request.Data[kCACHE_ARGUMENTS_KEY];
  if (!passedArgs.isNull()) {
    if (passedArgs.isString()) {
      cacheArgs.push_back(passedArgs.asString());
    } else if (passedArgs.isArray()) {
      for (auto const& arg : passedArgs) {
        if (!arg.isString()) {
          cacheArgumentsError = true;
          break;
        }
        cacheArgs.push_back(arg.asString());
      }
    } else {
      cacheArgumentsError = true;
    }
  }
  if (cacheArgumentsError) {
    request.ReportError(
      "cacheArguments must be unset, a string or an array of strings.");
  }

  std::string sourceDir = cm->GetHomeDirectory();
  const std::string buildDir = cm->GetHomeOutputDirectory();

  cmGlobalGenerator* gg = cm->GetGlobalGenerator();

  if (buildDir.empty()) {
    return request.ReportError("No build directory set via Handshake.");
  }

  if (cm->LoadCache(buildDir)) {
    // build directory has been set up before
    const char* cachedSourceDir =
      cm->GetState()->GetInitializedCacheValue("CMAKE_HOME_DIRECTORY");
    if (!cachedSourceDir) {
      return request.ReportError("No CMAKE_HOME_DIRECTORY found in cache.");
    }
    if (sourceDir.empty()) {
      sourceDir = std::string(cachedSourceDir);
      cm->SetHomeDirectory(sourceDir);
    }

    const char* cachedGenerator =
      cm->GetState()->GetInitializedCacheValue("CMAKE_GENERATOR");
    if (cachedGenerator) {
      if (gg && gg->GetName() != cachedGenerator) {
        return request.ReportError("Configured generator does not match with "
                                   "CMAKE_GENERATOR found in cache.");
      }
    }
  } else {
    // build directory has not been set up before
    if (sourceDir.empty()) {
      return request.ReportError("No sourceDirectory set via "
                                 "setGlobalSettings and no cache found in "
                                 "buildDirectory.");
    }
  }

  cmSystemTools::ResetErrorOccuredFlag(); // Reset error state

  if (cm->AddCMakePaths() != 1) {
    return request.ReportError("Failed to set CMake paths.");
  }

  if (!cm->SetCacheArgs(cacheArgs)) {
    return request.ReportError("cacheArguments could not be set.");
  }

  int ret = cm->Configure();
  if (ret < 0) {
    return request.ReportError("Configuration failed.");
  }

  std::vector<std::string> toWatchList;
  getCMakeInputs(gg, std::string(), buildDir, nullptr, &toWatchList, nullptr);

  FileMonitor()->MonitorPaths(toWatchList,
                              [this](const std::string& p, int e, int s) {
                                this->HandleCMakeFileChanges(p, e, s);
                              });

  m_State = STATE_CONFIGURED;
  m_isDirty = false;
  return request.Reply(Json::Value());
}

cmServerResponse cmServerProtocol1::ProcessGlobalSettings(
  const cmServerRequest& request)
{
  cmake* cm = this->CMakeInstance();
  Json::Value obj = Json::objectValue;

  // Capabilities information:
  obj[kCAPABILITIES_KEY] = cm->ReportCapabilitiesJson(true);

  obj[kDEBUG_OUTPUT_KEY] = cm->GetDebugOutput();
  obj[kTRACE_KEY] = cm->GetTrace();
  obj[kTRACE_EXPAND_KEY] = cm->GetTraceExpand();
  obj[kWARN_UNINITIALIZED_KEY] = cm->GetWarnUninitialized();
  obj[kWARN_UNUSED_KEY] = cm->GetWarnUnused();
  obj[kWARN_UNUSED_CLI_KEY] = cm->GetWarnUnusedCli();
  obj[kCHECK_SYSTEM_VARS_KEY] = cm->GetCheckSystemVars();

  obj[kSOURCE_DIRECTORY_KEY] = this->GeneratorInfo.SourceDirectory;
  obj[kBUILD_DIRECTORY_KEY] = this->GeneratorInfo.BuildDirectory;

  // Currently used generator:
  obj[kGENERATOR_KEY] = this->GeneratorInfo.GeneratorName;
  obj[kEXTRA_GENERATOR_KEY] = this->GeneratorInfo.ExtraGeneratorName;

  return request.Reply(obj);
}

static void setBool(const cmServerRequest& request, const std::string& key,
                    std::function<void(bool)> const& setter)
{
  if (request.Data[key].isNull()) {
    return;
  }
  setter(request.Data[key].asBool());
}

cmServerResponse cmServerProtocol1::ProcessSetGlobalSettings(
  const cmServerRequest& request)
{
  const std::vector<std::string> boolValues = {
    kDEBUG_OUTPUT_KEY,       kTRACE_KEY,       kTRACE_EXPAND_KEY,
    kWARN_UNINITIALIZED_KEY, kWARN_UNUSED_KEY, kWARN_UNUSED_CLI_KEY,
    kCHECK_SYSTEM_VARS_KEY
  };
  for (std::string const& i : boolValues) {
    if (!request.Data[i].isNull() && !request.Data[i].isBool()) {
      return request.ReportError("\"" + i +
                                 "\" must be unset or a bool value.");
    }
  }

  cmake* cm = this->CMakeInstance();

  setBool(request, kDEBUG_OUTPUT_KEY,
          [cm](bool e) { cm->SetDebugOutputOn(e); });
  setBool(request, kTRACE_KEY, [cm](bool e) { cm->SetTrace(e); });
  setBool(request, kTRACE_EXPAND_KEY, [cm](bool e) { cm->SetTraceExpand(e); });
  setBool(request, kWARN_UNINITIALIZED_KEY,
          [cm](bool e) { cm->SetWarnUninitialized(e); });
  setBool(request, kWARN_UNUSED_KEY, [cm](bool e) { cm->SetWarnUnused(e); });
  setBool(request, kWARN_UNUSED_CLI_KEY,
          [cm](bool e) { cm->SetWarnUnusedCli(e); });
  setBool(request, kCHECK_SYSTEM_VARS_KEY,
          [cm](bool e) { cm->SetCheckSystemVars(e); });

  return request.Reply(Json::Value());
}

cmServerResponse cmServerProtocol1::ProcessFileSystemWatchers(
  const cmServerRequest& request)
{
  const cmFileMonitor* const fm = FileMonitor();
  Json::Value result = Json::objectValue;
  Json::Value files = Json::arrayValue;
  for (auto const& f : fm->WatchedFiles()) {
    files.append(f);
  }
  Json::Value directories = Json::arrayValue;
  for (auto const& d : fm->WatchedDirectories()) {
    directories.append(d);
  }
  result[kWATCHED_FILES_KEY] = files;
  result[kWATCHED_DIRECTORIES_KEY] = directories;

  return request.Reply(result);
}

cmServerResponse cmServerProtocol1::ProcessCTests(
  const cmServerRequest& request)
{
  if (this->m_State < STATE_COMPUTED) {
    return request.ReportError("This instance was not yet computed.");
  }

  Json::Value result = Json::objectValue;
  result[kCONFIGURATIONS_KEY] =
    DumpCTestConfigurationsList(this->CMakeInstance());
  return request.Reply(result);
}

cmServerProtocol1::GeneratorInformation::GeneratorInformation(
  const std::string& generatorName, const std::string& extraGeneratorName,
  const std::string& toolset, const std::string& platform,
  const std::string& sourceDirectory, const std::string& buildDirectory)
  : GeneratorName(generatorName)
  , ExtraGeneratorName(extraGeneratorName)
  , Toolset(toolset)
  , Platform(platform)
  , SourceDirectory(sourceDirectory)
  , BuildDirectory(buildDirectory)
{
}

void cmServerProtocol1::GeneratorInformation::SetupGenerator(
  cmake* cm, std::string* errorMessage)
{
  const std::string fullGeneratorName =
    cmExternalMakefileProjectGenerator::CreateFullGeneratorName(
      GeneratorName, ExtraGeneratorName);

  cm->SetHomeDirectory(SourceDirectory);
  cm->SetHomeOutputDirectory(BuildDirectory);

  cmGlobalGenerator* gg = cm->CreateGlobalGenerator(fullGeneratorName);
  if (!gg) {
    setErrorMessage(
      errorMessage,
      std::string("Could not set up the requested combination of \"") +
        kGENERATOR_KEY + "\" and \"" + kEXTRA_GENERATOR_KEY + "\"");
    return;
  }

  cm->SetGlobalGenerator(gg);

  cm->SetGeneratorToolset(Toolset);
  cm->SetGeneratorPlatform(Platform);
}
