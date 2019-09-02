#include "command.hh"
#include "attr-path.hh"
#include "common-eval-args.hh"
#include "derivations.hh"
#include "eval-inline.hh"
#include "eval.hh"
#include "get-drvs.hh"
#include "store-api.hh"
#include "shared.hh"
#include "flake/flake.hh"
#include "flake/eval-cache.hh"

#include <regex>
#include <queue>

namespace nix {

MixFlakeOptions::MixFlakeOptions()
{
    mkFlag()
        .longName("recreate-lock-file")
        .description("recreate lock file from scratch")
        .set(&recreateLockFile, true);

    mkFlag()
        .longName("no-save-lock-file")
        .description("do not save the newly generated lock file")
        .set(&saveLockFile, false);

    mkFlag()
        .longName("no-registries")
        .description("don't use flake registries")
        .set(&useRegistries, false);
}

flake::HandleLockFile MixFlakeOptions::getLockFileMode()
{
    using namespace flake;
    return
        useRegistries
        ? recreateLockFile
          ? (saveLockFile ? RecreateLockFile : UseNewLockFile)
          : (saveLockFile ? UpdateLockFile : UseUpdatedLockFile)
        : AllPure;
}

SourceExprCommand::SourceExprCommand()
{
    mkFlag()
        .shortName('f')
        .longName("file")
        .label("file")
        .description("evaluate a set of attributes from FILE (deprecated)")
        .dest(&file);
}

ref<EvalState> EvalCommand::getEvalState()
{
    if (!evalState)
        evalState = std::make_shared<EvalState>(searchPath, getStore());
    return ref<EvalState>(evalState);
}

Buildable Installable::toBuildable()
{
    auto buildables = toBuildables();
    if (buildables.size() != 1)
        throw Error("installable '%s' evaluates to %d derivations, where only one is expected", what(), buildables.size());
    return std::move(buildables[0]);
}

App::App(EvalState & state, Value & vApp)
{
    state.forceAttrs(vApp);

    auto aType = vApp.attrs->need(state.sType);
    if (state.forceStringNoCtx(*aType.value, *aType.pos) != "app")
        throw Error("value does not have type 'app', at %s", *aType.pos);

    auto aProgram = vApp.attrs->need(state.symbols.create("program"));
    program = state.forceString(*aProgram.value, context, *aProgram.pos);

    // FIXME: check that 'program' is in the closure of 'context'.
    if (!state.store->isInStore(program))
        throw Error("app program '%s' is not in the Nix store", program);
}

App Installable::toApp(EvalState & state)
{
    return App(state, *toValue(state));
}

struct InstallableStorePath : Installable
{
    Path storePath;

    InstallableStorePath(const Path & storePath) : storePath(storePath) { }

    std::string what() override { return storePath; }

    Buildables toBuildables() override
    {
        return {{isDerivation(storePath) ? storePath : "", {{"out", storePath}}}};
    }

    std::optional<Path> getStorePath() override
    {
        return storePath;
    }
};

struct InstallableValue : Installable
{
    SourceExprCommand & cmd;

    InstallableValue(SourceExprCommand & cmd) : cmd(cmd) { }

    virtual std::vector<flake::EvalCache::Derivation> toDerivations()
    {
        auto state = cmd.getEvalState();

        auto v = toValue(*state);

        Bindings & autoArgs = *cmd.getAutoArgs(*state);

        DrvInfos drvInfos;
        getDerivations(*state, *v, "", autoArgs, drvInfos, false);

        std::vector<flake::EvalCache::Derivation> res;
        for (auto & drvInfo : drvInfos) {
            res.push_back({
                drvInfo.queryDrvPath(),
                drvInfo.queryOutPath(),
                drvInfo.queryOutputName()
            });
        }

        return res;
    }

    Buildables toBuildables() override
    {
        Buildables res;

        PathSet drvPaths;

        for (auto & drv : toDerivations()) {
            Buildable b{drv.drvPath};
            drvPaths.insert(b.drvPath);

            auto outputName = drv.outputName;
            if (outputName == "")
                throw Error("derivation '%s' lacks an 'outputName' attribute", b.drvPath);

            b.outputs.emplace(outputName, drv.outPath);

            res.push_back(std::move(b));
        }

        // Hack to recognize .all: if all drvs have the same drvPath,
        // merge the buildables.
        if (drvPaths.size() == 1) {
            Buildable b{*drvPaths.begin()};
            for (auto & b2 : res)
                b.outputs.insert(b2.outputs.begin(), b2.outputs.end());
            return {b};
        } else
            return res;
    }
};

struct InstallableExpr : InstallableValue
{
    std::string text;

    InstallableExpr(SourceExprCommand & cmd, const std::string & text)
         : InstallableValue(cmd), text(text) { }

    std::string what() override { return text; }

    Value * toValue(EvalState & state) override
    {
        auto v = state.allocValue();
        state.eval(state.parseExprFromString(text, absPath(".")), *v);
        return v;
    }
};

struct InstallableAttrPath : InstallableValue
{
    Value * v;
    std::string attrPath;

    InstallableAttrPath(SourceExprCommand & cmd, Value * v, const std::string & attrPath)
        : InstallableValue(cmd), v(v), attrPath(attrPath)
    { }

    std::string what() override { return attrPath; }

    Value * toValue(EvalState & state) override
    {
        auto vRes = findAlongAttrPath(state, attrPath, *cmd.getAutoArgs(state), *v);
        state.forceValue(*vRes);
        return vRes;
    }
};

void makeFlakeClosureGCRoot(Store & store,
    const FlakeRef & origFlakeRef,
    const flake::ResolvedFlake & resFlake)
{
    if (std::get_if<FlakeRef::IsPath>(&origFlakeRef.data)) return;

    /* Get the store paths of all non-local flakes. */
    PathSet closure;

    assert(store.isValidPath(resFlake.flake.sourceInfo.storePath));
    closure.insert(resFlake.flake.sourceInfo.storePath);

    std::queue<std::reference_wrapper<const flake::LockedInputs>> queue;
    queue.push(resFlake.lockFile);

    while (!queue.empty()) {
        const flake::LockedInputs & flake = queue.front();
        queue.pop();
        /* Note: due to lazy fetching, these paths might not exist
           yet. */
        for (auto & dep : flake.inputs) {
            auto path = dep.second.computeStorePath(store);
            if (store.isValidPath(path))
                closure.insert(path);
            queue.push(dep.second);
        }
    }

    if (closure.empty()) return;

    /* Write the closure to a file in the store. */
    auto closurePath = store.addTextToStore("flake-closure", concatStringsSep(" ", closure), closure);

    Path cacheDir = getCacheDir() + "/nix/flake-closures";
    createDirs(cacheDir);

    auto s = origFlakeRef.to_string();
    assert(s[0] != '.');
    s = replaceStrings(s, "%", "%25");
    s = replaceStrings(s, "/", "%2f");
    s = replaceStrings(s, ":", "%3a");
    Path symlink = cacheDir + "/" + s;
    debug("writing GC root '%s' for flake closure of '%s'", symlink, origFlakeRef);
    replaceSymlink(closurePath, symlink);
    store.addIndirectRoot(symlink);
}

struct InstallableFlake : InstallableValue
{
    FlakeRef flakeRef;
    Strings attrPaths;
    Strings prefixes;

    InstallableFlake(SourceExprCommand & cmd, FlakeRef && flakeRef, Strings attrPaths)
        : InstallableValue(cmd), flakeRef(flakeRef), attrPaths(std::move(attrPaths))
    { }

    InstallableFlake(SourceExprCommand & cmd, FlakeRef && flakeRef,
        std::string attrPath, Strings && prefixes)
        : InstallableValue(cmd), flakeRef(flakeRef), attrPaths{attrPath},
          prefixes(prefixes)
    { }

    std::string what() override { return flakeRef.to_string() + ":" + *attrPaths.begin(); }

    std::vector<std::string> getActualAttrPaths()
    {
        std::vector<std::string> res;

        for (auto & prefix : prefixes)
            res.push_back(prefix + *attrPaths.begin());

        for (auto & s : attrPaths)
            res.push_back(s);

        return res;
    }

    Value * getFlakeOutputs(EvalState & state, const flake::ResolvedFlake & resFlake)
    {
        auto vFlake = state.allocValue();

        callFlake(state, resFlake, *vFlake);

        makeFlakeClosureGCRoot(*state.store, flakeRef, resFlake);

        auto vOutputs = (*vFlake->attrs->get(state.symbols.create("outputs")))->value;

        state.forceValue(*vOutputs);

        return vOutputs;
    }

    std::vector<flake::EvalCache::Derivation> toDerivations() override
    {
        auto state = cmd.getEvalState();

        auto resFlake = resolveFlake(*state, flakeRef, cmd.getLockFileMode());

        Value * vOutputs = nullptr;

        auto emptyArgs = state->allocBindings(0);

        auto & evalCache = flake::EvalCache::singleton();

        auto fingerprint = resFlake.getFingerprint();

        for (auto & attrPath : getActualAttrPaths()) {
            auto drv = evalCache.getDerivation(fingerprint, attrPath);
            if (drv) {
                if (state->store->isValidPath(drv->drvPath))
                    return {*drv};
            }

            if (!vOutputs)
                vOutputs = getFlakeOutputs(*state, resFlake);

            try {
                auto * v = findAlongAttrPath(*state, attrPath, *emptyArgs, *vOutputs);
                state->forceValue(*v);

                auto drvInfo = getDerivation(*state, *v, false);
                if (!drvInfo)
                    throw Error("flake output attribute '%s' is not a derivation", attrPath);

                auto drv = flake::EvalCache::Derivation{
                    drvInfo->queryDrvPath(),
                    drvInfo->queryOutPath(),
                    drvInfo->queryOutputName()
                };

                evalCache.addDerivation(fingerprint, attrPath, drv);

                return {drv};
            } catch (AttrPathNotFound & e) {
            }
        }

        throw Error("flake '%s' does not provide attribute %s",
            flakeRef, concatStringsSep(", ", quoteStrings(attrPaths)));
    }

    Value * toValue(EvalState & state) override
    {
        auto resFlake = resolveFlake(state, flakeRef, cmd.getLockFileMode());

        auto vOutputs = getFlakeOutputs(state, resFlake);

        auto emptyArgs = state.allocBindings(0);

        for (auto & attrPath : getActualAttrPaths()) {
            try {
                auto * v = findAlongAttrPath(state, attrPath, *emptyArgs, *vOutputs);
                state.forceValue(*v);
                return v;
            } catch (AttrPathNotFound & e) {
            }
        }

        throw Error("flake '%s' does not provide attribute %s",
            flakeRef, concatStringsSep(", ", quoteStrings(attrPaths)));
    }
};

// FIXME: extend
std::string attrRegex = R"([A-Za-z_][A-Za-z0-9-_+]*)";
static std::regex attrPathRegex(fmt(R"(%1%(\.%1%)*)", attrRegex));

std::vector<std::shared_ptr<Installable>> SourceExprCommand::parseInstallables(
    ref<Store> store, std::vector<std::string> ss)
{
    std::vector<std::shared_ptr<Installable>> result;

    if (file) {
        // FIXME: backward compatibility hack
        evalSettings.pureEval = false;

        auto state = getEvalState();
        auto vFile = state->allocValue();
        state->evalFile(lookupFileArg(*state, *file), *vFile);

        if (ss.empty())
            ss = {""};

        for (auto & s : ss)
            result.push_back(std::make_shared<InstallableAttrPath>(*this, vFile, s));

    } else {

        auto follow = [&](const std::string & s) -> std::optional<Path> {
            try {
                return store->followLinksToStorePath(s);
            } catch (NotInStore &) {
                return {};
            }
        };

        for (auto & s : ss) {

            size_t colon;
            std::optional<Path> storePath;

            if (s.compare(0, 1, "(") == 0)
                result.push_back(std::make_shared<InstallableExpr>(*this, s));

            else if (hasPrefix(s, "nixpkgs.")) {
                bool static warned;
                warnOnce(warned, "the syntax 'nixpkgs.<attr>' is deprecated; use 'nixpkgs:<attr>' instead");
                result.push_back(std::make_shared<InstallableFlake>(*this, FlakeRef("nixpkgs"),
                        Strings{"legacyPackages." + std::string(s, 8)}));
            }

            else {

                std::exception_ptr flakeEx;

                try {
                    auto flakeRef = FlakeRef(s, true);
                    result.push_back(std::make_shared<InstallableFlake>(
                            *this, std::move(flakeRef), getDefaultFlakeAttrPaths()));
                    continue;
                } catch (MissingFlake &) {
                    /* 's' could be parsed as a flakeref, but it
                       references a local path that is not a flake. So
                       take note of that. */
                    flakeEx = std::current_exception();
                } catch (BadFlakeRef &) {
                }

                if ((colon = s.rfind(':')) != std::string::npos) {
                    auto flakeRef = std::string(s, 0, colon);
                    auto attrPath = std::string(s, colon + 1);
                    result.push_back(std::make_shared<InstallableFlake>(
                            *this,
                            FlakeRef(flakeRef, true),
                            attrPath,
                            getDefaultFlakeAttrPathPrefixes()));
                }

                else if (s.find('/') != std::string::npos && (storePath = follow(s)))
                    result.push_back(std::make_shared<InstallableStorePath>(*storePath));

                else {
                    if (flakeEx)
                        std::rethrow_exception(flakeEx);
                    else
                        throw Error("unsupported argument '%s'", s);
                }
            }
        }
    }

    return result;
}

std::shared_ptr<Installable> SourceExprCommand::parseInstallable(
    ref<Store> store, const std::string & installable)
{
    auto installables = parseInstallables(store, {installable});
    assert(installables.size() == 1);
    return installables.front();
}

Buildables build(ref<Store> store, RealiseMode mode,
    std::vector<std::shared_ptr<Installable>> installables)
{
    if (mode != Build)
        settings.readOnlyMode = true;

    Buildables buildables;

    PathSet pathsToBuild;

    for (auto & i : installables) {
        for (auto & b : i->toBuildables()) {
            if (b.drvPath != "") {
                StringSet outputNames;
                for (auto & output : b.outputs)
                    outputNames.insert(output.first);
                pathsToBuild.insert(
                    b.drvPath + "!" + concatStringsSep(",", outputNames));
            } else
                for (auto & output : b.outputs)
                    pathsToBuild.insert(output.second);
            buildables.push_back(std::move(b));
        }
    }

    if (mode == DryRun)
        printMissing(store, pathsToBuild, lvlError);
    else if (mode == Build)
        store->buildPaths(pathsToBuild);

    return buildables;
}

PathSet toStorePaths(ref<Store> store, RealiseMode mode,
    std::vector<std::shared_ptr<Installable>> installables)
{
    PathSet outPaths;

    for (auto & b : build(store, mode, installables))
        for (auto & output : b.outputs)
            outPaths.insert(output.second);

    return outPaths;
}

Path toStorePath(ref<Store> store, RealiseMode mode,
    std::shared_ptr<Installable> installable)
{
    auto paths = toStorePaths(store, mode, {installable});

    if (paths.size() != 1)
        throw Error("argument '%s' should evaluate to one store path", installable->what());

    return *paths.begin();
}

PathSet toDerivations(ref<Store> store,
    std::vector<std::shared_ptr<Installable>> installables, bool useDeriver)
{
    PathSet drvPaths;

    for (auto & i : installables)
        for (auto & b : i->toBuildables()) {
            if (b.drvPath.empty()) {
                if (!useDeriver)
                    throw Error("argument '%s' did not evaluate to a derivation", i->what());
                for (auto & output : b.outputs) {
                    auto derivers = store->queryValidDerivers(output.second);
                    if (derivers.empty())
                        throw Error("'%s' does not have a known deriver", i->what());
                    // FIXME: use all derivers?
                    drvPaths.insert(*derivers.begin());
                }
            } else
                drvPaths.insert(b.drvPath);
        }

    return drvPaths;
}

void InstallablesCommand::prepare()
{
    if (_installables.empty() && !file && useDefaultInstallables())
        // FIXME: commands like "nix install" should not have a
        // default, probably.
        _installables.push_back(".");
    installables = parseInstallables(getStore(), _installables);
}

void InstallableCommand::prepare()
{
    installable = parseInstallable(getStore(), _installable);
}

}
