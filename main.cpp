#include <iostream>
#include <fstream>
#include <stdint.h>

#define SIZE_OUTPUT_BUF 150 * 1024 * 1024 // 150MB

struct game_chart_index
{
	uint32_t pos_chart;
	uint32_t size_chart;
};

struct game_chart_header
{
	game_chart_index chart_index[12];
};

struct game_chart_event
{
	int32_t timecode; // milliseconds?
	int8_t command;
	int8_t val1;
	int16_t val2;
};

struct dummy_wave_header
{
	char riff[4]; // 'RIFF'
	uint32_t size_wave;
	char wave[4]; // 'WAVE'
	char fmt[4]; // 'fmt '
	uint32_t size_fmt_chunk; // 0x10
	uint16_t audio_format; // 1 = PCM
	uint16_t channels; // 2 = stereo
	uint32_t samplerate; // 44100;
	uint32_t byterate; // 176400
	uint16_t blockalign; // 4
	uint16_t bitpersample; // 0x10(16)
	char data[4]; // 'data'
	uint32_t size_data;
};

inline int32_t mixing_function(int32_t buf, int16_t input)
{
#if 1
	return buf + input;
#else
	// definitly not working with multi keysounds mixing
	return buf + input - (buf * input / 32768);
#endif
}

// mix input with buffer, treat buffer as 32bit signed samples for further operatings
int32_t add_wave_to_buffer(uint8_t *output_buffer, uint8_t *wave_buffer, int32_t size_input, int32_t size_buffer, int32_t mix_offset)
{
	// treat as 44100Hz, 32bit, stereo, sample offset = (mix_offset * 441 / 10) * sizeof(int32_t) * 2
	int32_t pos_mix = (mix_offset * 441 / 10) * sizeof(int32_t)* 2, pos = 0;
	int32_t *sample_mix = (int32_t*)(output_buffer + pos_mix);
	int16_t *sample_in = (int16_t*)wave_buffer;
	while (pos < size_input)
	{
		// mix both channels
		*sample_mix = mixing_function(*sample_mix, *sample_in);
		*(sample_mix + 1) = mixing_function(*(sample_mix + 1), *(sample_in + 1));
		sample_mix += 2; sample_in += 2;
		pos += 4;
	}
	return (pos_mix + size_input * 2 > size_buffer) ? pos_mix + size_input * 2 : size_buffer;
}

// 32bit signed back to 16bit signed
int32_t normalize_bgm(uint8_t *output_buf, int32_t size_output_buf)
{
	int32_t *sample_mix = (int32_t*)output_buf;
	int16_t *write_sample = (int16_t*)output_buf;

	// get the max sample value
	uint32_t max_sample_val = 0;
	while ((int)sample_mix - (int)output_buf < size_output_buf)
	{
		if (max_sample_val < abs(*sample_mix))
			max_sample_val = abs(*sample_mix);
		sample_mix++;
	}

	// scale all sample based on max value
	sample_mix = (int32_t*)output_buf;
	float ratio = (32768 / (max_sample_val / 1.0));
	if (ratio > 1.0) ratio = 1.0;
	printf("\t\tmixing level %f\n", ratio);
	while ((int)sample_mix - (int)output_buf < size_output_buf)
	{
		*sample_mix = (int32_t)(*sample_mix * ratio);
		if (*sample_mix > 32767)
			*write_sample = 32767;
		else if (*sample_mix < -32768)
			*write_sample = -32768;
		else
			*write_sample = *sample_mix;
		sample_mix++;
		write_sample++;
	}
	return size_output_buf / 2;
}

// only for input as 16bit 44100Hz stereo PCM
int32_t mix_bgm(uint8_t *output_buf, int32_t size_output_buf, char *keysound_prefix, int16_t keysound_id, int32_t time)
{
//	printf("\tadding keysound %08d at %08d\n", keysound_id, time);
//	printf("\t%08d\t%08d\n", time, keysound_id);
	if (keysound_id == 0)
		return size_output_buf;
	char path_keysound[260];
	sprintf_s(path_keysound, 260, "%s_%08d.wav", keysound_prefix, keysound_id - 1);
	std::fstream keysound;
	keysound.open(path_keysound, std::ios::in | std::ios::binary);
	if (!keysound)
		return size_output_buf;

	keysound.seekg(0, std::ios::end);
	size_t size_keysound = keysound.tellg();
	keysound.seekg(0, std::ios::beg);

	unsigned char *key_buf = new unsigned char[size_keysound];
	keysound.read((char*)key_buf, size_keysound);
	keysound.close();

	uint32_t *pseek = (uint32_t*)key_buf;
	while (*pseek != 0x61746164 // just seek data trunk, i don't want to deal with RIFF header shits
		&& (int)pseek - (int)key_buf < size_keysound - 4)
		pseek = (uint32_t*)((char*)pseek + 1);

	if ((int)pseek - (int)key_buf >= size_keysound - 4)
	{
		delete[]key_buf;
		return size_output_buf;
	}

	pseek++;

	int32_t ret = add_wave_to_buffer(output_buf, (uint8_t*)(pseek + 1), *pseek, size_output_buf, time);
	delete[]key_buf;
	return ret;
}

int16_t keysounds[8 * 2]; // 8 lanes each side
int32_t bss_end_pos[2];

int process_chart_file(char *path, char *keysound_prefix)
{
	std::fstream gamechart;
	gamechart.open(path, std::ios::in | std::ios::binary);
	if (!gamechart) return -1;
	gamechart.seekg(0, std::ios::end);
	size_t size_chart = gamechart.tellg();
	gamechart.seekg(0, std::ios::beg);

	unsigned char *chartdata = new unsigned char[size_chart];
	gamechart.read((char*)chartdata, size_chart);
	gamechart.close();

	game_chart_header *p_header = (game_chart_header*)chartdata;

	int i = 0;
	game_chart_event *p_chart = NULL;
	dummy_wave_header dummyheader;
	memcpy(dummyheader.riff, "RIFF", 4);
	memcpy(dummyheader.wave, "WAVE", 4);
	memcpy(dummyheader.fmt, "fmt ", 4);
	memcpy(dummyheader.data, "data", 4);
	dummyheader.size_fmt_chunk = 16;
	dummyheader.audio_format = 1;
	dummyheader.channels = 2;
	dummyheader.samplerate = 44100;
	dummyheader.byterate = dummyheader.samplerate * dummyheader.channels * sizeof(int16_t);// 176400
	dummyheader.blockalign = dummyheader.channels * sizeof(int16_t);// 4
	dummyheader.bitpersample = 8 * sizeof(int16_t);// 16

	unsigned char *output_buf = new unsigned char[SIZE_OUTPUT_BUF];
	int j = 0, total_keysound = 0;
	for (i = 0; i < 12; i++)
	{
		if (p_header->chart_index[i].size_chart != 0)
		{
			p_chart = (game_chart_event*)(chartdata + p_header->chart_index[i].pos_chart);
			// create a new sound buffer
			int32_t size_output_buf = 0;
			memset(output_buf, 0, SIZE_OUTPUT_BUF);

			memset(keysounds, 0, sizeof(keysounds));
			memset(bss_end_pos, -1, sizeof(bss_end_pos));
			total_keysound = 0;
			while ((int)p_chart - (int)chartdata < p_header->chart_index[i].size_chart + p_header->chart_index[i].pos_chart)
			{
				if (p_chart->timecode == 0x7fffffff) break;
//				printf("%8d: %02X, %02X, %04X\n", p_chart->timecode, p_chart->command, p_chart->val1, p_chart->val2);

				// check bss end point
				for (j = 0; j < 2; j++)
				{
					if (bss_end_pos[j] != -1 && p_chart->timecode > bss_end_pos[j])
					{
						total_keysound++;
						size_output_buf = mix_bgm(output_buf, size_output_buf, keysound_prefix, keysounds[7 + j * 8], bss_end_pos[j]);
						bss_end_pos[j] = -1;
					}
				}

				switch (p_chart->command)
				{
				case 00:
				case 01:
					// mix keysound of keysounds[p_chart->val1]
					size_output_buf = mix_bgm(output_buf, size_output_buf, keysound_prefix, keysounds[p_chart->val1 + 8 * (p_chart->command & 1)], p_chart->timecode);
					total_keysound++;
					if (p_chart->val1 == 07 && p_chart->val2 != 0)
					{
						// lane 0x07 CN == BSS
#if 0
						// is this correct? i saw some song has some weird bss/cn length rather than other chart which will have 1ms offset different causing different generated files, so idk
						bss_end_pos[0 + (p_chart->command & 1)] = p_chart->val2 + p_chart->timecode - 1;
#else
						bss_end_pos[0 + (p_chart->command & 1)] = p_chart->val2 + p_chart->timecode;
#endif
					}
					break;
				case 02:
				case 03:
					keysounds[p_chart->val1 + 8 * (p_chart->command & 1)] = p_chart->val2;
					if (p_chart->val1 == 07 && bss_end_pos[0 + (p_chart->command & 1)] != -1 && p_chart->timecode == bss_end_pos[0 + (p_chart->command & 1)])
					{
						// i don't know if it's correct but let's try to put this shit here
						total_keysound++;
						size_output_buf = mix_bgm(output_buf, size_output_buf, keysound_prefix, keysounds[7 + (p_chart->command & 1) * 8], bss_end_pos[0 + (p_chart->command & 1)]);
						bss_end_pos[0 + (p_chart->command & 1)] = -1;
					}
					break;
				case 07:
					// mix keysound of p_chart->val2
					size_output_buf = mix_bgm(output_buf, size_output_buf, keysound_prefix, p_chart->val2, p_chart->timecode);
					total_keysound++;
					break;
				default:
					break;
				}

				p_chart++;
			}

			// write sound buffer to file
			size_output_buf = normalize_bgm(output_buf, size_output_buf);
			printf("\t%s_%02d: %d\n", path, i, total_keysound);
			dummyheader.size_data = size_output_buf;
			dummyheader.size_wave = size_output_buf + 36;
			std::fstream output;
			char output_path[260];
			sprintf_s(output_path, 260, "%s_%02d.wav", path, i);
			output.open(output_path, std::ios::out | std::ios::binary | std::ios::trunc);
			if (output)
			{
				output.write((char*)&dummyheader, sizeof(dummyheader));
				output.write((char*)output_buf, size_output_buf);
				output.close();
			}
		}
	}
	delete[]output_buf;

	delete[]chartdata;
	return 0;
}

int main(int argc, char *argv[])
{
	if (argc < 3)
		return -1;
	return process_chart_file(argv[1], argv[2]);
}
