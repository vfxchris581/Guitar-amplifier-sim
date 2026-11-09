#include "plugin/IRLibrary.h"

const juce::StringArray& IRLibrary::categories()
{
    static const juce::StringArray c { "1x12", "2x12", "4x12", "Open-back", "Closed-back",
                                       "Vintage", "Modern", "Metal", "Rock", "Clean" };
    return c;
}

juce::File IRLibrary::defaultFolder()
{
    if (folderOverride != juce::File())
        return folderOverride;
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
        .getChildFile (GAS_PRODUCT_NAME)
        .getChildFile ("Impulse Responses");
}

IRLibrary::IRLibrary (juce::File folder) : root (std::move (folder))
{
    rescan();
}

void IRLibrary::loadIndex()
{
    tagIndex.clear();
    const auto json = juce::JSON::parse (root.getChildFile ("library.json"));
    if (auto* obj = json.getDynamicObject())
        for (const auto& p : obj->getProperties())
            tagIndex.set (p.name, p.value.toString());
}

void IRLibrary::saveIndex() const
{
    auto obj = juce::DynamicObject::Ptr (new juce::DynamicObject());
    for (const auto& p : tagIndex)
        obj->setProperty (p.name, p.value);
    root.createDirectory();
    root.getChildFile ("library.json").replaceWithText (juce::JSON::toString (juce::var (obj.get())));
}

void IRLibrary::rescan()
{
    loadIndex();
    all.clear();
    if (! root.isDirectory())
        return;
    for (const auto& f : root.findChildFiles (juce::File::findFiles, true, "*.wav;*.aif;*.aiff"))
        all.add ({ f, tagsFor (f) });
    std::sort (all.begin(), all.end(), [] (const Entry& a, const Entry& b) { return a.name().compareNatural (b.name()) < 0; });
}

juce::StringArray IRLibrary::tagsFor (const juce::File& f) const
{
    juce::StringArray t;
    t.addTokens (tagIndex[juce::Identifier (key (f))].toString(), "|", {});
    t.removeEmptyStrings();
    return t;
}

void IRLibrary::setTags (const juce::File& f, const juce::StringArray& tags)
{
    tagIndex.set (juce::Identifier (key (f)), tags.joinIntoString ("|"));
    saveIndex();
    for (auto& e : all)
        if (e.file == f)
            e.tags = tags;
}

juce::Array<IRLibrary::Entry> IRLibrary::filtered (const juce::StringArray& required, const juce::String& search) const
{
    juce::Array<Entry> out;
    for (const auto& e : all)
    {
        bool ok = true;
        for (const auto& t : required)
            ok = ok && e.tags.contains (t);
        if (ok && search.isNotEmpty())
            ok = e.name().containsIgnoreCase (search) || e.tags.joinIntoString (" ").containsIgnoreCase (search);
        if (ok)
            out.add (e);
    }
    return out;
}

int IRLibrary::importFiles (const juce::Array<juce::File>& files, juce::StringArray& skipped)
{
    root.createDirectory();
    int n = 0;
    for (const auto& f : files)
    {
        if (! f.hasFileExtension ("wav;aif;aiff"))
        {
            skipped.add (f.getFileName() + " (not a WAV/AIFF file)");
            continue;
        }
        auto dest = root.getChildFile (f.getFileName());
        if (dest.exists())
            dest = dest.getNonexistentSibling();
        if (f.copyFileTo (dest))
            ++n;
        else
            skipped.add (f.getFileName());
    }
    rescan();
    return n;
}
