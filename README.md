# Audio Monitor filter for OBS Studio

Plugin for OBS Studio to add Audio Monitor filter.
It allows you to put the audio of a OBS source to an audio device by adding the Audio Monitor filter on the Source in OBS Studio.

![Screenshot](media/screenshot.png)

This can be useful in different use cases:

   * monitoring a source on multiple devices
   * monitoring audio without the delays of syncing to video
   * separate monitoring audio levels per source and device
   
# Build
- Build OBS Studio: https://obsproject.com/wiki/Install-Instructions
- Check out this repository to plugins/audio-monitor
- Add `add_subdirectory(audio-monitor)` to plugins/CMakeLists.txt
- Rebuild OBS Studio

# Donations
https://www.paypal.me/exeldro
