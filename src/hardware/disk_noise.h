#ifndef DOSBOX_DISK_NOISE_H
#define DOSBOX_DISK_NOISE_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class MixerChannel;

class DiskNoiseDevice {
public:
	DiskNoiseDevice(const std::string& spin_sample_path,
	                const std::string& seek_sample_path,
	                const std::string& spin_channel_name,
	                const std::string& seek_channel_name);

	void ActivateSpin();
	void PlaySeek();

private:
	void LoadSample(const std::string& path, std::vector<int16_t>& buffer);
	void AudioCallbackSpin(const int len);
	void AudioCallbackSeek(/*uint8_t* stream,*/ const int len);

	std::vector<int16_t> spin_sample_;
	std::vector<int16_t> seek_sample_;
	int spin_pos_ = 0;
	int seek_pos_ = 0;
	int last_activity_ = 0;

	std::shared_ptr<MixerChannel> spin_channel_;
	std::shared_ptr<MixerChannel> seek_channel_;

	static constexpr int spinup_fade_ms = 300;
};

// Expose the disk noise devices to be able to affect them from hdd/fdd code
extern std::unique_ptr<DiskNoiseDevice> floppy_noise;
extern std::unique_ptr<DiskNoiseDevice> hdd_noise;

#endif // DOSBOX_DISK_NOISE_H
