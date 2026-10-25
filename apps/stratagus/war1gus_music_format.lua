-- Shared Linux/ESP music selection. Keep movies and speech in their original
-- audio format; only the numbered Warcraft I music tracks are switchable.
if preferences.MusicFormat == nil then preferences.MusicFormat = "ogg" end
if preferences.MidiSoundFont == nil then preferences.MidiSoundFont = "TimGM6mb.sf2" end
-- Reloading the game scripts must not wrap an earlier wrapper recursively.
War1gusNativePlayMusic = War1gusNativePlayMusic or PlayMusic
local nativePlayMusic = War1gusNativePlayMusic
local lastTrack
local function chosenFormat()
  return preferences.MusicFormat == "midi" and "midi" or "ogg"
end
local function trackPath(name, format)
  local number = string.match(name or "", "^music/(%d%d)%.[^/]+$")
  if not number then return name end
  return "music/" .. number .. (format == "midi" and ".mid" or ".ogg")
end
local function musicFiles()
  local files = {}
  for _, file in ipairs(ListFilesInDirectory("music")) do
    files[file] = true
  end
  return files
end
function war1gus.GetSoundFonts()
  local fonts = {}
  for file in pairs(musicFiles()) do
    if string.lower(file):match("%.sf2$") and not file:find("[/\\]") and not file:find("..", 1, true) then
      fonts[#fonts + 1] = file
    end
  end
  table.sort(fonts)
  return fonts
end
local function applyFont(name)
  if type(name) ~= "string" or #name > 180 or not name:lower():match("%.sf2$")
    or name:find("[/\\]") or name:find("..", 1, true) then
    return false, "Invalid soundfont filename"
  end
  if not musicFiles()[name] then return false, "Missing music/" .. name end
  if not SetMidiSoundFont then return false, "Engine lacks soundfont selection support" end
  if not SetMidiSoundFont("music/" .. name) then
    return false, "Soundfont rejected (invalid bank or device size limit)"
  end
  return true
end
function war1gus.SetSoundFont(name)
  local ok, message = applyFont(name)
  if not ok then return false, message end
  local previous = preferences.MidiSoundFont
  preferences.MidiSoundFont = name
  if chosenFormat() == "midi" and lastTrack and IsMusicEnabled() then
    StopMusic()
    if PlayMusic(lastTrack) < 0 then
      preferences.MidiSoundFont = previous
      applyFont(previous)
      PlayMusic(lastTrack)
      return false, "Music decoder unavailable"
    end
  end
  SavePreferences()
  return true
end
function PlayMusic(name)
  if string.match(name or "", "^music/%d%d%.") then lastTrack = name end
  return nativePlayMusic(trackPath(name, chosenFormat()))
end
function war1gus.SetMusicFormat(format)
  if format ~= "ogg" and format ~= "midi" then return false, "Unknown music format" end
  local files = musicFiles()
  if format == "midi" then
    local ok, message = applyFont(preferences.MidiSoundFont)
    if not ok then return false, message end
  end
  local extension = format == "midi" and ".mid" or ".ogg"
  for i = 0, 44 do
    local number = (i < 10 and "0" or "") .. tostring(i)
    if not files[number .. extension] then
      return false, "Missing music/" .. number .. extension
    end
  end
  local previous = chosenFormat()
  preferences.MusicFormat = format
  if lastTrack and IsMusicEnabled() then
    StopMusic()
    if PlayMusic(lastTrack) < 0 then
      preferences.MusicFormat = previous
      PlayMusic(lastTrack)
      return false, "Music decoder unavailable"
    end
  end
  war1gus.music_extension = extension
  SavePreferences()
  return true
end
-- Initialize the playlist/briefing/result paths to the saved preference.
war1gus.music_extension = chosenFormat() == "midi" and ".mid" or ".ogg"
-- Set the engine's bank before the first numbered track is opened. Old or
-- removed selections fall back to the installed default, not an arbitrary bank.
if SetMidiSoundFont and not applyFont(preferences.MidiSoundFont) then
  preferences.MidiSoundFont = "TimGM6mb.sf2"
  applyFont(preferences.MidiSoundFont)
end
