#include "plugin/Library.h"

#include "json.hpp"

#include <fstream>

namespace
{
juce::File documents()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile (GAS_PRODUCT_NAME);
}
} // namespace

// ===========================================================================
juce::File ModelLibrary::root() { return rootOverride != juce::File() ? rootOverride : documents(); }
juce::File ModelLibrary::modelsFolder() { return root().getChildFile ("Models"); }
juce::File ModelLibrary::capturesFolder() { return root().getChildFile ("Captures"); }

ModelLibrary::ModelLibrary() { rescan(); }

ModelLibrary::Entry ModelLibrary::readFile (const juce::File& f)
{
    Entry e;
    e.file = f;
    e.fileName = f.getFileNameWithoutExtension();
    try
    {
        std::ifstream in (f.getFullPathName().toStdString(), std::ios::binary);
        const auto j = nlohmann::json::parse (in);
        auto str = [] (const nlohmann::json& o, const char* k) -> juce::String
        {
            auto it = o.find (k);
            return it != o.end() && it->is_string() ? juce::String (it->get<std::string>()) : juce::String();
        };
        e.architecture = str (j, "architecture");
        if (auto sr = j.find ("sample_rate"); sr != j.end() && sr->is_number())
            e.sampleRate = sr->get<double>();
        if (auto m = j.find ("metadata"); m != j.end() && m->is_object())
        {
            e.name = str (*m, "name");
            e.modeledBy = str (*m, "modeled_by");
            e.gearMake = str (*m, "gear_make");
            e.gearModel = str (*m, "gear_model");
            e.gearType = str (*m, "gear_type");
            e.toneType = str (*m, "tone_type");
            if (auto t = m->find ("training"); t != m->end() && t->is_object())
                if (auto v = t->find ("validation_esr"); v != t->end() && v->is_number())
                    e.validationEsr = v->get<double>();
        }
    }
    catch (...)
    {
        e.architecture = "(unreadable)";
    }
    return e;
}

void ModelLibrary::rescan()
{
    index = juce::JSON::parse (root().getChildFile ("models.json"));
    if (! index.isObject())
        index = juce::var (new juce::DynamicObject());
    all.clear();
    for (const auto& folder : { modelsFolder(), capturesFolder() })
        if (folder.isDirectory())
            for (const auto& f : folder.findChildFiles (juce::File::findFiles, true, "*.nam"))
            {
                auto e = readFile (f);
                e.ownCapture = f.isAChildOf (capturesFolder());
                const auto& r = record (f);
                e.displayName = r["displayName"].toString();
                if (e.displayName.isEmpty())
                    e.displayName = e.name.isNotEmpty() ? e.name : e.fileName;
                e.favourite = (bool) r["favourite"];
                e.collections.addTokens (r["collections"].toString(), "|", {});
                e.collections.removeEmptyStrings();
                e.tags.addTokens (r["tags"].toString(), "|", {});
                e.tags.removeEmptyStrings();
                e.description = r["description"].toString();
                all.add (e);
            }
    std::sort (all.begin(), all.end(), [] (const Entry& a, const Entry& b) { return a.displayName.compareNatural (b.displayName) < 0; });
}

juce::var& ModelLibrary::record (const juce::File& f)
{
    auto* obj = index.getDynamicObject();
    const juce::Identifier key (f.getRelativePathFrom (root()));
    if (! obj->hasProperty (key))
        obj->setProperty (key, juce::var (new juce::DynamicObject()));
    return *obj->getProperties().getVarPointer (key);
}

void ModelLibrary::save() const
{
    root().createDirectory();
    root().getChildFile ("models.json").replaceWithText (juce::JSON::toString (index));
}

juce::Array<ModelLibrary::Entry> ModelLibrary::filtered (const Filter& f) const
{
    juce::Array<Entry> out;
    for (const auto& e : all)
    {
        if (f.favouritesOnly && ! e.favourite) continue;
        if (f.gearType.isNotEmpty() && e.gearType != f.gearType) continue;
        if (f.toneType.isNotEmpty() && e.toneType != f.toneType) continue;
        if (f.collection.isNotEmpty() && ! e.collections.contains (f.collection)) continue;
        if (f.search.isNotEmpty())
        {
            const auto hay = e.displayName + " " + e.fileName + " " + e.gearMake + " " + e.gearModel + " " + e.modeledBy + " "
                             + e.tags.joinIntoString (" ") + " " + e.description;
            if (! hay.containsIgnoreCase (f.search)) continue;
        }
        out.add (e);
    }
    return out;
}

juce::StringArray ModelLibrary::allCollections() const
{
    juce::StringArray c;
    for (const auto& e : all)
        c.addArray (e.collections);
    c.removeDuplicates (true);
    c.sortNatural();
    return c;
}

const ModelLibrary::Entry* ModelLibrary::find (const juce::File& f) const
{
    for (const auto& e : all)
        if (e.file == f)
            return &e;
    return nullptr;
}

int ModelLibrary::importFiles (const juce::Array<juce::File>& files, juce::StringArray& problems)
{
    modelsFolder().createDirectory();
    int n = 0;
    for (const auto& f : files)
    {
        if (! f.hasFileExtension ("nam"))
        {
            problems.add (f.getFileName() + ": not a .nam file");
            continue;
        }
        if (readFile (f).architecture.isEmpty() || readFile (f).architecture == "(unreadable)")
        {
            problems.add (f.getFileName() + ": not a valid NAM model");
            continue;
        }
        auto dest = modelsFolder().getChildFile (f.getFileName());
        if (dest.exists())
            dest = dest.getNonexistentSibling();
        f.copyFileTo (dest) ? ++n : (problems.add (f.getFileName() + ": copy failed"), 0);
    }
    rescan();
    return n;
}

bool ModelLibrary::remove (const juce::File& f, juce::String& error)
{
    if (! (f.isAChildOf (modelsFolder()) || f.isAChildOf (capturesFolder())))
    {
        error = "Only models inside the library can be deleted.";
        return false;
    }
    if (! f.deleteFile())
    {
        error = "Could not delete " + f.getFullPathName();
        return false;
    }
    index.getDynamicObject()->removeProperty (juce::Identifier (f.getRelativePathFrom (root())));
    save();
    rescan();
    return true;
}

bool ModelLibrary::exportTo (const juce::File& model, const juce::File& dest, juce::String& error)
{
    if (! model.copyFileTo (dest))
    {
        error = "Could not write " + dest.getFullPathName();
        return false;
    }
    return true;
}

void ModelLibrary::setDisplayName (const juce::File& f, const juce::String& v) { record (f).getDynamicObject()->setProperty ("displayName", v); save(); rescan(); }
void ModelLibrary::setFavourite (const juce::File& f, bool v) { record (f).getDynamicObject()->setProperty ("favourite", v); save(); rescan(); }
void ModelLibrary::setCollections (const juce::File& f, const juce::StringArray& v) { record (f).getDynamicObject()->setProperty ("collections", v.joinIntoString ("|")); save(); rescan(); }
void ModelLibrary::setTags (const juce::File& f, const juce::StringArray& v) { record (f).getDynamicObject()->setProperty ("tags", v.joinIntoString ("|")); save(); rescan(); }
void ModelLibrary::setDescription (const juce::File& f, const juce::String& v) { record (f).getDynamicObject()->setProperty ("description", v); save(); rescan(); }

// ===========================================================================
juce::File PresetLibrary::folder() { return folderOverride != juce::File() ? folderOverride : documents().getChildFile ("Presets"); }

PresetLibrary::PresetLibrary() { rescan(); }

void PresetLibrary::rescan()
{
    favourites.clear();
    favourites.addTokens (folder().getChildFile ("favourites.txt").loadFileAsString(), "\n", {});
    favourites.trim();
    favourites.removeEmptyStrings();
    all.clear();
    if (folder().isDirectory())
        for (const auto& f : folder().findChildFiles (juce::File::findFiles, false, juce::String ("*") + extension))
            all.add ({ f, favourites.contains (f.getFileName()) });
    std::sort (all.begin(), all.end(), [] (const Preset& a, const Preset& b) { return a.name().compareNatural (b.name()) < 0; });
}

juce::File PresetLibrary::save (const juce::String& name, const juce::MemoryBlock& state, juce::String& error)
{
    const auto safe = juce::File::createLegalFileName (name.trim());
    if (safe.isEmpty())
    {
        error = "Please give the preset a name.";
        return {};
    }
    folder().createDirectory();
    const auto f = folder().getChildFile (safe + extension);
    if (! f.replaceWithData (state.getData(), state.getSize()))
    {
        error = "Could not write " + f.getFullPathName();
        return {};
    }
    rescan();
    return f;
}

bool PresetLibrary::load (const juce::File& f, juce::MemoryBlock& state, juce::String& error) const
{
    if (! f.loadFileAsData (state) || state.getSize() < 8)
    {
        error = "Could not read " + f.getFileName();
        return false;
    }
    return true;
}

bool PresetLibrary::rename (const juce::File& f, const juce::String& newName, juce::String& error)
{
    const auto dest = folder().getChildFile (juce::File::createLegalFileName (newName.trim()) + extension);
    if (dest.exists())
    {
        error = "A preset with that name already exists.";
        return false;
    }
    const bool fav = favourites.contains (f.getFileName());
    if (! f.moveFileTo (dest))
    {
        error = "Could not rename the preset.";
        return false;
    }
    if (fav)
        setFavourite (f, false), setFavourite (dest, true);
    rescan();
    return true;
}

bool PresetLibrary::remove (const juce::File& f)
{
    const bool ok = f.deleteFile();
    setFavourite (f, false);
    rescan();
    return ok;
}

void PresetLibrary::setFavourite (const juce::File& f, bool fav)
{
    favourites.removeString (f.getFileName());
    if (fav)
        favourites.add (f.getFileName());
    folder().createDirectory();
    folder().getChildFile ("favourites.txt").replaceWithText (favourites.joinIntoString ("\n"));
    rescan();
}

int PresetLibrary::importFiles (const juce::Array<juce::File>& files)
{
    folder().createDirectory();
    int n = 0;
    for (const auto& f : files)
        if (f.hasFileExtension (juce::String (extension).substring (1)))
        {
            auto dest = folder().getChildFile (f.getFileName());
            if (dest.exists())
                dest = dest.getNonexistentSibling();
            n += f.copyFileTo (dest) ? 1 : 0;
        }
    rescan();
    return n;
}
