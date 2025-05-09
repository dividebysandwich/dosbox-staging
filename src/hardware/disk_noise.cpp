#include "disk_noise.h"
#include "mixer.h"
#include "setup.h"
#include "timer.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>


// Load a disk noise sample. Expects a WAV file with 16-bit PCM data at 44100 Hz.
void DiskNoiseDevice::LoadSample(const std::string& path, std::vector<int16_t>& buffer)
{
	std::ifstream file(path, std::ios::binary);
	if (!file.is_open()) {
		LOG_WARNING("DiskNoiseDevice: Failed to open %s", path.c_str());
		return;
	}

    // Skip 44 bytes of the WAV header
	char header[44];
	file.read(header, 44);

	std::vector<int16_t> temp;
	int16_t sample;
	while (file.read(reinterpret_cast<char*>(&sample), sizeof(sample))) {
		temp.push_back(sample);
	}
	LOG_INFO("DiskNoiseDevice: Loaded %zu samples from %s", temp.size(), path.c_str());
	buffer = std::move(temp);
}

DiskNoiseDevice::DiskNoiseDevice(const std::string& spin_sample_path,
                                 const std::string& seek_sample_path,
                                 const std::string& spin_channel_name,
                                 const std::string& seek_channel_name)
        : spin_sample_(),
          seek_sample_(),
          spin_pos_(0),
          seek_pos_(0),
          last_activity_(0),
          spin_channel_(nullptr),
          seek_channel_(nullptr)
{
    LOG_INFO("DiskNoiseDevice: Loading samples: %s, %s",
             spin_sample_path.c_str(),
             seek_sample_path.c_str());
	LoadSample(spin_sample_path, spin_sample_);
	LoadSample(seek_sample_path, seek_sample_);

	using namespace std::placeholders;

    MIXER_LockMixerThread();

//	auto spin_callback = [this](int frames) {
//		std::vector<uint8_t> buffer(frames * sizeof(int16_t));
//		AudioCallbackSpin(buffer.data(), buffer.size());
//	};
	auto spin_callback = std::bind(&DiskNoiseDevice::AudioCallbackSpin,
							  this,
							  std::placeholders::_1);

	auto seek_callback = std::bind(&DiskNoiseDevice::AudioCallbackSeek,
							  this,
							  std::placeholders::_1);

	spin_channel_ = MIXER_AddChannel(spin_callback,
	                                 44100,
	                                 spin_channel_name.c_str(),
	                                 {ChannelFeature::Stereo,
                                      ChannelFeature::DigitalAudio});
	seek_channel_ = MIXER_AddChannel(seek_callback,
	                                 44100,
	                                 seek_channel_name.c_str(),
	                                 {ChannelFeature::Stereo,
                                      ChannelFeature::DigitalAudio});

                                     
	// auto callback = std::bind(&CDROM_Interface_Physical::CdAudioCallback,
	//                           this,
	//                           std::placeholders::_1);
	// mixer_channel = MIXER_AddChannel(callback,
	//                                  REDBOOK_PCM_FRAMES_PER_SECOND,
	//                                  name.c_str(),
	//                                  {ChannelFeature::Stereo,
	//                                   ChannelFeature::DigitalAudio});

	MIXER_LockMixerThread();
	spin_channel_->Enable(false);
	seek_channel_->Enable(false);

    // Automatically start spinning for HDDs
    if (spin_channel_name == "HDD_SPIN") {
        ActivateSpin();
    }
}

void DiskNoiseDevice::ActivateSpin()
{
    LOG_INFO("DiskNoiseDevice: Activating spin");

    last_activity_ = GetTicks();
	spin_channel_->Enable(true);
}

void DiskNoiseDevice::PlaySeek()
{
	seek_pos_ = 0;
	seek_channel_->Enable(true);
}

void DiskNoiseDevice::AudioCallbackSpin(const int len)
//void DiskNoiseDevice::AudioCallbackSpin(uint8_t* stream, const int len)
{
	const int frames = len / sizeof(int16_t);
	const int now    = GetTicks();
	const int fade   = (spinup_fade_ms * 44100) / 1000;

	LOG_INFO("DiskNoiseDevice: AudioCallbackSpin: %d frames", frames);

	std::vector<int16_t> out(frames);

	for (int i = 0; i < frames; ++i) {
		if (spin_sample_.empty()) {
			out[i] = 0;
			continue;
		}

		float gain = 1.0f;
		if (now - last_activity_ < spinup_fade_ms) {
			const int frame_age = i + ((now - last_activity_) * 44100) /
			                                  1000;
			gain = std::min(1.0f, static_cast<float>(frame_age) / fade);
		}

		out[i] = static_cast<int16_t>(spin_sample_[spin_pos_] * gain);
		if (++spin_pos_ >= spin_sample_.size()) {
			spin_pos_ = 0;
		}
	}
	spin_channel_->AddSamples_s16(frames, out.data()); 
}

void DiskNoiseDevice::AudioCallbackSeek(const int len)
{
	const int frames = len / sizeof(int16_t);

    std::vector<int16_t> out(frames);

	for (int i = 0; i < frames; ++i) {
		out[i] = (seek_pos_ < seek_sample_.size())
		               ? seek_sample_[seek_pos_++]
		               : 0;
	}

	if (seek_pos_ >= seek_sample_.size()) {
		seek_channel_->Enable(false);
	}
    seek_channel_->AddSamples_s16(frames, out.data()); 
}


// void DiskNoiseDevice::AudioCallbackSeek(uint8_t* stream, const int len)
// {
// 	auto* out        = reinterpret_cast<int16_t*>(stream);
// 	const int frames = len / sizeof(int16_t);

// 	for (int i = 0; i < frames; ++i) {
// 		out[i] = (seek_pos_ < seek_sample_.size())
// 		               ? seek_sample_[seek_pos_++]
// 		               : 0;
// 	}

// 	if (seek_pos_ >= seek_sample_.size()) {
// 		seek_channel_->Enable(false);
// 	}
// //    seek_channel_->AddSamples_s16(frames, out); 
// }

void DiskNoiseDevice::Shutdown()
{
	LOG_INFO("DiskNoiseDevice: Shutting down");
	spin_channel_->Enable(false);
	seek_channel_->Enable(false);
	spin_channel_.reset();
	seek_channel_.reset();
}


std::unique_ptr<DiskNoiseDevice> floppy_noise = nullptr;
std::unique_ptr<DiskNoiseDevice> hdd_noise = nullptr;

void DISKNOISE_Init(Section* section)
{
    assert(section);
    const auto prop = static_cast<Section_prop*>(section);

	const std::string floppy_spin = prop->Get_string("floppy_spin_sample");
	const std::string floppy_seek = prop->Get_string("floppy_seek_sample");
	const std::string hdd_spin    = prop->Get_string("hdd_spin_sample");
	const std::string hdd_seek    = prop->Get_string("hdd_seek_sample");

	if (!floppy_spin.empty() || !floppy_seek.empty()) {
		floppy_noise = std::make_unique<DiskNoiseDevice>(
			floppy_spin,
			floppy_seek,
			"FLOPPY_SPIN",
			"FLOPPY_SEEK");
	}

	if (!hdd_spin.empty() || !hdd_seek.empty()) {
		hdd_noise = std::make_unique<DiskNoiseDevice>(
			hdd_spin,
			hdd_seek,
			"HDD_SPIN",
			"HDD_SEEK");
	}

}


void DISKNOISE_ShutDown([[maybe_unused]] Section* section)
{
    // TODO: Fix Cleanup
    MIXER_LockMixerThread();
	floppy_noise->Shutdown();
    floppy_noise.reset();
	hdd_noise.reset();
    MIXER_UnlockMixerThread();
}

void init_disknoise_dosbox_settings(Section_prop& secprop)
{
	constexpr auto when_idle = Property::Changeable::OnlyAtStart;

	auto* str_prop = secprop.Add_string("floppy_spin_sample", when_idle, "");
	assert(str_prop);
	str_prop->Set_help(
	        "Path to a .wav file for the floppy spin noise emulation.\n"
	        "Leave empty to not play this noise.\n"
            "The file should be a 16-bit PCM WAV file at 44100 Hz.\n");

    str_prop = secprop.Add_string("floppy_seek_sample", when_idle, "");
	assert(str_prop);
	str_prop->Set_help(
	        "Path to a .wav file for the floppy seek noise emulation.\n"
	        "Leave empty to not play this noise.\n"
            "The file should be a 16-bit PCM WAV file at 44100 Hz.\n");

   	str_prop = secprop.Add_string("hdd_spin_sample", when_idle, "");
	assert(str_prop);
	str_prop->Set_help(
	        "Path to a .wav file for the hard disk spin noise emulation.\n"
	        "Leave empty to not play this noise.\n"
            "The file should be a 16-bit PCM WAV file at 44100 Hz.\n");

    str_prop = secprop.Add_string("hdd_seek_sample", when_idle, "");
	assert(str_prop);
	str_prop->Set_help(
	        "Path to a .wav file for the hard disk seek noise emulation.\n"
	        "Leave empty to not play this noise.\n"
            "The file should be a 16-bit PCM WAV file at 44100 Hz.\n");
    
}

void DISKNOISE_AddConfigSection(const ConfigPtr& conf)
{
	assert(conf);

	constexpr auto changeable_at_runtime = false; // TODO: Deal with runtime changes

	Section_prop* sec = conf->AddSection_prop("disknoise", &DISKNOISE_Init, changeable_at_runtime);
	assert(sec);
	init_disknoise_dosbox_settings(*sec);
}
