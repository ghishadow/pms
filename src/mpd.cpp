/* vi:set ts=8 sts=8 sw=8:
 *
 * Practical Music Search
 * Copyright (c) 2006-2011  Kim Tore Jensen
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "mpd.h"
#include "console.h"
#include "window.h"
#include "config.h"
#include "field.h"
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netdb.h>
#include <cstring>
#include <string>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <math.h>

using namespace std;

extern Config config;
extern Windowmanager wm;
extern Fieldtypes fieldtypes;
extern MPD mpd;

void update_library_statusbar()
{
	unsigned int percent;
	percent = round((mpd.library.size() / mpd.stats.songs) * 100);
	stinfo("Retrieving library %d%%%% ...", percent);
}

MPD::MPD()
{
	errno = 0;
	error = "";
	host = "";
	port = "";
	buffer = "";
	sock = 0;
	connected = false;
	is_idle = false;
	memset(&last_update, 0, sizeof last_update);
	memset(&last_clock, 0, sizeof last_clock);
	memset(&status, 0, sizeof status);
	memset(&stats, 0, sizeof stats);
}

bool MPD::set_idle(bool nidle)
{
	if (nidle == is_idle)
		return false;
	
	if (nidle)
	{
		mpd_raw_send("idle");
		is_idle = true;
		return true;
	}

	mpd_raw_send("noidle");
	mpd_getline(NULL);

	return true;
}

bool MPD::trigerr(int nerrno, const char * format, ...)
{
	va_list		ap;
	char		buffer[1024];

	va_start(ap, format);
	vsprintf(buffer, format, ap);
	va_end(ap);

	error = buffer;
	errno = nerrno;

	sterr("MPD: %s", buffer);

	return false;
}

bool MPD::mpd_connect(string nhost, string nport)
{
	int			status;
	char			buf[32];
	struct addrinfo		hints;
	struct addrinfo *	res;

	host = nhost;
	port = nport;

	if (connected)
		mpd_disconnect();

	stinfo("Connecting to server '%s' port '%s'...", host.c_str(), port.c_str());

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	if ((status = getaddrinfo(host.c_str(), port.c_str(), &hints, &res)) != 0)
	{
		trigerr(MPD_ERR_CONNECTION, "getaddrinfo error: %s", gai_strerror(status));
		return false;
	}

	sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (sock == -1)
	{
		trigerr(MPD_ERR_CONNECTION, "could not create socket!");
		freeaddrinfo(res);
		return false;
	}

	if (connect(sock, res->ai_addr, res->ai_addrlen) == -1)
	{
		trigerr(MPD_ERR_CONNECTION, "could not connect to %s:%s", host.c_str(), port.c_str());
		close(sock);
		freeaddrinfo(res);
		return false;
	}

	freeaddrinfo(res);
	connected = true;

	FD_ZERO(&fdset);
	FD_SET(sock, &fdset);
	FD_SET(STDIN_FILENO, &fdset);

	stinfo("Connected to server '%s' on port '%s'.", host.c_str(), port.c_str());
	recv(sock, &buf, 32, 0);
	set_protocol_version(buf);
	is_idle = false;

	return connected;
}

void MPD::mpd_disconnect()
{
	close(sock);
	sock = 0;
	connected = false;
	is_idle = false;
	buffer.clear();
	trigerr(MPD_ERR_CONNECTION, "Connection to MPD server closed.");
}

bool MPD::is_connected()
{
	return connected;
}

bool MPD::set_password(string password)
{
	if (!connected)
		return false;

	if (password.size() == 0)
		return true;
	
	mpd_send("password \"%s\"", password.c_str());
	if (mpd_getline(NULL) == MPD_GETLINE_OK)
	{
		stinfo("Password '%s' accepted by server.", password.c_str());
		return true;
	}

	return true;
}

bool MPD::set_protocol_version(string data)
{
	unsigned int i = 7;
	unsigned int last = 7;
	int pos = 0;

	if (data.substr(0, 7) != "OK MPD ")
		return false;

	while (i <= data.size() && pos < 3)
	{
		if (data[i] == '.' || data[i] == '\n')
		{
			protocol_version[pos] = atoi(data.substr(last, i - last).c_str());
			++pos;
			last = i + 1;
		}
		++i;
	}
	debug("MPD server speaking protocol version %d.%d.%d", protocol_version[0], protocol_version[1], protocol_version[2]);

	return true;
}

int MPD::mpd_send(const char * data, ...)
{
	va_list		ap;
	char		buffer[1024];

	va_start(ap, data);
	vsprintf(buffer, data, ap);
	va_end(ap);

	if (!connected)
		return -1;

	set_idle(false);
	return mpd_raw_send(buffer);
}

int MPD::mpd_raw_send(string data)
{
	unsigned int sent;
	int s;

	if (!connected)
		return -1;

	data += '\n';
	if ((s = send(sock, data.c_str(), data.size(), 0)) == -1)
		return s;

	sent = s;

	while (sent < data.size())
	{
		if ((s = send(sock, data.substr(sent).c_str(), data.size() - sent, 0)) == -1)
			return -1;

		sent += s;
	}

	// Raw traffic dump
	//debug("-> %s", data.c_str());

	return sent;
}

int MPD::mpd_getline(string * nextline)
{
	char buf[1025];
	int received = 0;
	int s;
	size_t pos;
	struct timeval timeout;
	fd_set set;
	string line = "";

	if (!connected)
		return MPD_GETLINE_ERR;

	memset(&timeout, 0, sizeof timeout);
	timeout.tv_usec = 10;
	FD_ZERO(&set);
	FD_SET(sock, &set);

	if ((s = select(sock+1, &set, NULL, NULL, &timeout)) == -1)
	{
		debug("Oops! mpd_getline() called, but select() returns an error.", NULL);
		return MPD_GETLINE_ERR;
	}

	is_idle = false;

	while(buffer.size() == 0 || buffer.find('\n') == string::npos)
	{
		received = recv(sock, &buf, 1024, 0);
		if (received == 0)
		{
			mpd_disconnect();
			return MPD_GETLINE_ERR;
		}
		else if (received == -1)
		{
			continue;
		}
		buf[received] = '\0';
		buffer += buf;
	}

	if ((pos = buffer.find('\n')) != string::npos)
	{
		line = buffer.substr(0, pos);
		if (buffer.size() == pos + 1)
			buffer = "";
		else
			buffer = buffer.substr(pos + 1);
	}

	if (line.size() == 0)
		return MPD_GETLINE_ERR;

	// Raw traffic dump
	//debug("<- %s", line.c_str());

	if (line == "OK")
		return MPD_GETLINE_OK;

	if (line.size() >= 3 && line.substr(0, 3) == "ACK")
	{
		trigerr(MPD_ERR_ACK, buf);
		return MPD_GETLINE_ACK;
	}

	if (nextline != NULL)
		*nextline = line;

	return MPD_GETLINE_MORE;
}

int MPD::split_pair(string * line, string * param, string * value)
{
	size_t pos;

	if ((pos = line->find(':')) != string::npos)
	{
		*param = line->substr(0, pos);
		*value = line->substr(pos + 2);
		return true;
	}

	return false;
}

int MPD::recv_songs_to_list(Songlist * slist, void (*func) ())
{
	Song * song = NULL;
	Field * field;
	string buf;
	string param;
	string value;
	int status;
	unsigned int count = 0;

	while((status = mpd_getline(&buf)) == MPD_GETLINE_MORE)
	{
		if (!split_pair(&buf, &param, &value))
			continue;

		field = fieldtypes.find_mpd(param);
		if (field == NULL)
		{
			//debug("Unhandled song metadata field '%s' in response from MPD", param.c_str());
			continue;
		}

		if (field->type == FIELD_FILE)
		{
			if (song != NULL)
			{
				song->init();
				slist->add(song);
				if (++count % 100 == 0)
					func();
			}

			song = new Song;
		}

		if (song != NULL)
			song->f[field->type] = value;
	}

	if (song != NULL)
	{
		song->init();
		slist->add(song);
	}

	return status;
}

int MPD::get_playlist()
{
	int s;

	/* Ignore duplicate update messages */
	if (playlist.version == status.playlist)
		return false;

	playlist.truncate(status.playlistlength);
	if (playlist.version == -1)
		mpd_send("playlistinfo");
	else
		mpd_send("plchanges %d", playlist.version);

	s = recv_songs_to_list(&playlist, NULL);

	playlist.version = status.playlist;
	wm.playlist->draw();

	debug("Playlist has been updated to version %d", playlist.version);

	return s;
}

int MPD::get_library()
{
	int s;

	get_stats();

	if ((unsigned long long)library.version == stats.db_update)
	{
		debug("Request for library update, but local copy is the same as server.", NULL);
		return MPD_GETLINE_OK;
	}

	library.truncate(stats.songs);

	mpd_send("listallinfo");
	s = recv_songs_to_list(&library, update_library_statusbar);
	library.version = stats.db_update;
	wm.library->draw();

	debug("Library has been received, total %d songs.", library.size());

	return s;
}

int MPD::get_stats()
{
	string buf;
	string param;
	string value;
	int s;

	mpd_send("stats");

	while((s = mpd_getline(&buf)) == MPD_GETLINE_MORE)
	{
		if (!split_pair(&buf, &param, &value))
			continue;

		if (param == "artists")
			stats.songs = atol(value.c_str());
		else if (param == "albums")
			stats.albums = atoll(value.c_str());
		else if (param == "songs")
			stats.songs = atoll(value.c_str());
		else if (param == "uptime")
			stats.uptime = atoll(value.c_str());
		else if (param == "playtime")
			stats.playtime = atoll(value.c_str());
		else if (param == "db_playtime")
			stats.db_playtime = atoll(value.c_str());
		else if (param == "db_update")
			stats.db_update = atoll(value.c_str());
	}

	return s;
}

int MPD::get_status()
{
	string buf;
	string param;
	string value;
	int s;
	size_t pos;

	mpd_send("status");

	while((s = mpd_getline(&buf)) == MPD_GETLINE_MORE)
	{
		if (!split_pair(&buf, &param, &value))
			continue;

		if (param == "volume")
			status.volume = atoi(value.c_str());
		else if (param == "repeat")
			status.repeat = atoi(value.c_str());
		else if (param == "random")
			status.random = atoi(value.c_str());
		else if (param == "single")
			status.single = atoi(value.c_str());
		else if (param == "consume")
			status.consume = atoi(value.c_str());
		else if (param == "playlist")
			status.playlist = atoi(value.c_str());
		else if (param == "playlistlength")
			status.playlistlength = atoi(value.c_str());
		else if (param == "xfade")
			status.xfade = atoi(value.c_str());
		else if (param == "mixrampdb")
			status.mixrampdb = atof(value.c_str());
		else if (param == "mixrampdelay")
			status.mixrampdelay = atoi(value.c_str());
		else if (param == "song")
			status.song = atol(value.c_str());
		else if (param == "songid")
			status.songid = atol(value.c_str());
		else if (param == "elapsed")
			status.elapsed = atof(value.c_str());
		else if (param == "bitrate")
			status.bitrate = atoi(value.c_str());
		else if (param == "nextsong")
			status.nextsong = atol(value.c_str());
		else if (param == "nextsongid")
			status.nextsongid = atol(value.c_str());

		else if (param == "state")
		{
			if (value == "play")
				status.state = MPD_STATE_PLAY;
			else if (value == "stop")
				status.state = MPD_STATE_STOP;
			else if (value == "pause")
				status.state = MPD_STATE_PAUSE;
			else
				status.state = MPD_STATE_UNKNOWN;
		}

		else if (param == "time")
		{
			if ((pos = value.find(':')) != string::npos)
			{
				status.elapsed = atof(value.substr(0, pos).c_str());
				status.length = atoi(value.substr(pos + 1).c_str());
			}
		}

		else if (param == "audio")
		{
			if ((pos = value.find(':')) != string::npos)
			{
				status.samplerate = atoi(value.substr(0, pos).c_str());
				status.bits = atoi(value.substr(pos + 1).c_str());
				if ((pos = value.find(':', pos + 1)) != string::npos)
				{
					status.channels = atoi(value.substr(pos + 1).c_str());
				}
			}
		}
	}

	gettimeofday(&last_update, NULL);
	memcpy(&last_clock, &last_update, sizeof last_clock);

	return s;
}

void MPD::run_clock()
{
	struct timeval tm;
	gettimeofday(&tm, NULL);

	if (status.state == MPD_STATE_PLAY)
	{
		status.elapsed += (tm.tv_sec - last_clock.tv_sec);
		status.elapsed += (tm.tv_usec - last_clock.tv_usec) / 1000000.000000;
	}

	memcpy(&last_clock, &tm, sizeof last_clock);
}

int MPD::poll()
{
	string line;
	string param;
	string value;
	int updates;
	fd_set set;
	struct timeval timeout;
	int s;

	set_idle(true);

	memset(&timeout, 0, sizeof timeout);
	memcpy(&set, &fdset, sizeof set);
	timeout.tv_sec = 1;
	if ((s = select(sock+1, &set, NULL, NULL, &timeout)) == -1)
	{
		mpd_disconnect();
		return false;
	}
	else if (s == 0)
	{
		// no data ready to recv(), but let's update our clock
		run_clock();
		return false;
	}

	if (!FD_ISSET(sock, &set))
		return true;

	updates = MPD_UPDATE_NONE;

	while((s = mpd_getline(&line)) == MPD_GETLINE_MORE)
	{
		if (!split_pair(&line, &param, &value))
			continue;

		if (param != "changed")
			continue;

		if (value == "database")
		{
			// the song database has been modified after update. 
			updates |= MPD_UPDATE_LIBRARY;
		}
		else if (value == "update")
		{
			// a database update has started or finished. If the database was modified during the update, the database event is also emitted. 
		}
		else if (value == "stored_playlist")
		{
			// a stored playlist has been modified, renamed, created or deleted 
		}
		else if (value == "playlist")
		{
			// the current playlist has been modified 
			updates |= MPD_UPDATE_STATUS;
			updates |= MPD_UPDATE_PLAYLIST;
		}
		else if (value == "player")
		{
			// the player has been started, stopped or seeked
			updates |= MPD_UPDATE_STATUS;
		}
		else if (value == "mixer")
		{
			// the volume has been changed 
			updates |= MPD_UPDATE_STATUS;
		}
		else if (value == "output")
		{
			// an audio output has been enabled or disabled 
		}
		else if (value == "options")
		{
			// options like repeat, random, crossfade, replay gain
			updates |= MPD_UPDATE_STATUS;
		}
		else if (value == "sticker")
		{
			// the sticker database has been modified.
		}
		else if (value == "subscription")
		{
			// a client has subscribed or unsubscribed to a channel
		}
		else if (value == "message")
		{
			// a message was received on a channel this client is subscribed to; this event is only emitted when the queue is empty
		}
	}

	if (updates & MPD_UPDATE_STATUS)
		get_status();
	if (updates & MPD_UPDATE_PLAYLIST)
		get_playlist();
	if (updates & MPD_UPDATE_LIBRARY)
		get_library();

	set_idle(true);

	return true;
}

int MPD::set_consume(bool nconsume)
{
	mpd_send("consume %d", nconsume);
	return (mpd_getline(NULL) == MPD_GETLINE_OK);
}

int MPD::set_crossfade(unsigned int nseconds)
{
	mpd_send("crossfade %d", nseconds);
	return (mpd_getline(NULL) == MPD_GETLINE_OK);
}

int MPD::set_mixrampdb(int ndecibels)
{
	return false;
}

int MPD::set_mixrampdelay(int nseconds)
{
	return false;
}

int MPD::set_random(bool nrandom)
{
	mpd_send("random %d", nrandom);
	return (mpd_getline(NULL) == MPD_GETLINE_OK);
}

int MPD::set_repeat(bool nrepeat)
{
	mpd_send("repeat %d", nrepeat);
	return (mpd_getline(NULL) == MPD_GETLINE_OK);
}

int MPD::set_volume(unsigned int nvol)
{
	mpd_send("setvol %d", nvol);
	return (mpd_getline(NULL) == MPD_GETLINE_OK);
}

int MPD::set_single(bool nsingle)
{
	mpd_send("single %d", nsingle);
	return (mpd_getline(NULL) == MPD_GETLINE_OK);
}

int MPD::set_replay_gain_mode(replay_gain_mode nrgm)
{
	return false;
}

int MPD::pause(bool npause)
{
	mpd_send("pause %d", npause);
	return (mpd_getline(NULL) == MPD_GETLINE_OK);
}
