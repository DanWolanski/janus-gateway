/*! \file   janus_videoroom.c
 * \author Lorenzo Miniero <lorenzo@meetecho.com>
 * \copyright GNU General Public License v3
 * \brief  Janus VideoRoom plugin
 * \details  This is a plugin implementing a videoconferencing SFU
 * (Selective Forwarding Unit) for Janus, that is an audio/video router.
 * This means that the plugin implements a virtual conferencing room peers
 * can join and leave at any time. This room is based on a Publish/Subscribe
 * pattern. Each peer can publish his/her own live audio/video feeds: this
 * feed becomes an available stream in the room the other participants can
 * attach to. This means that this plugin allows the realization of several
 * different scenarios, ranging from a simple webinar (one speaker, several
 * watchers) to a fully meshed video conference (each peer sending and
 * receiving to and from all the others).
 *
 * Considering that this plugin allows for several different WebRTC PeerConnections
 * to be on at the same time for the same peer (specifically, each peer
 * potentially has 1 PeerConnection on for publishing and N on for subscriptions
 * from other peers), each peer may need to attach several times to the same
 * plugin for every stream: this means that each peer needs to have at least one
 * handle active for managing its relation with the plugin (joining a room,
 * leaving a room, muting/unmuting, publishing, receiving events), and needs
 * to open a new one each time he/she wants to subscribe to a feed from
 * another publisher participant. The handle used for a subscription,
 * however, would be logically a "slave" to the master one used for
 * managing the room: this means that it cannot be used, for instance,
 * to unmute in the room, as its only purpose would be to provide a
 * context in which creating the recvonly PeerConnection for the
 * subscription to an active publisher participant.
 *
 * \note Work is going on to implement SSRC multiplexing (Unified Plan),
 * meaning that in the future you'll be able to use the same
 * Janus handle/VideoRoom subscriber/PeerConnection to receive multiple
 * publishers at the same time.
 *
 * Rooms to make available are listed in the plugin configuration file.
 * A pre-filled configuration file is provided in \c conf/janus.plugin.videoroom.cfg
 * and includes a demo room for testing. The same plugin is also used
 * dynamically (that is, with rooms created on the fly via API) in the
 * Screen Sharing demo as well.
 *
 * To add more rooms or modify the existing one, you can use the following
 * syntax:
 *
 * \verbatim
[<unique room ID>]
description = This is my awesome room
is_private = yes|no (private rooms don't appear when you do a 'list' request)
secret = <optional password needed for manipulating (e.g. destroying) the room>
pin = <optional password needed for joining the room>
require_pvtid = yes|no (whether subscriptions are required to provide a valid
             a valid private_id to associate with a publisher, default=no)
publishers = <max number of concurrent senders> (e.g., 6 for a video
             conference or 1 for a webinar, default=3)
bitrate = <max video bitrate for senders> (e.g., 128000)
fir_freq = <send a FIR to publishers every fir_freq seconds> (0=disable)
audiocodec = opus|g722|pcmu|pcma|isac32|isac16 (audio codec to force on publishers, default=opus
			can be a comma separated list in order of preference, e.g., opus,pcmu)
videocodec = vp8|vp9|h264 (video codec to force on publishers, default=vp8
			can be a comma separated list in order of preference, e.g., vp9,vp8,h264)
video_svc = yes|no (whether SVC support must be enabled; works only for VP9, default=no)
audiolevel_ext = yes|no (whether the ssrc-audio-level RTP extension must be
	negotiated/used or not for new publishers, default=yes)
audiolevel_event = yes|no (whether to emit event to other users or not)
audio_active_packets = 100 (number of packets with audio level, default=100, 2 seconds)
audio_level_average = 25 (average value of audio level, 127=muted, 0='too loud', default=25)
videoorient_ext = yes|no (whether the video-orientation RTP extension must be
	negotiated/used or not for new publishers, default=yes)
playoutdelay_ext = yes|no (whether the playout-delay RTP extension must be
	negotiated/used or not for new publishers, default=yes)
transport_wide_cc_ext = yes|no (whether the transport wide CC RTP extension must be
	negotiated/used or not for new publishers, default=no note that this currently
	doesn't work correctly when the publisher is doing simulcasting)
record = true|false (whether this room should be recorded, default=false)
rec_dir = <folder where recordings should be stored, when enabled>
notify_joining = true|false (optional, whether to notify all participants when a new
            participant joins the room. The Videoroom plugin by design only notifies
            new feeds (publishers), and enabling this may result extra notification
            traffic. This flag is particularly useful when enabled with \c require_pvtid
            for admin to manage listening only participants. default=false)
\endverbatim
 *
 * Note that recording will work with all codecs except iSAC.
 *
 * \section sfuapi Video Room API
 *
 * The Video Room API supports several requests, some of which are
 * synchronous and some asynchronous. There are some situations, though,
 * (invalid JSON, invalid request) which will always result in a
 * synchronous error response even for asynchronous requests. 
 *
 * \c create , \c destroy , \c edit , \c exists, \c list, \c allowed, \c kick and
 * and \c listparticipants are synchronous requests, which means you'll
 * get a response directly within the context of the transaction.
 * \c create allows you to create a new video room dynamically, as an
 * alternative to using the configuration file; \c edit allows you to
 * dynamically edit some room properties (e.g., the PIN); \c destroy removes a
 * video room and destroys it, kicking all the users out as part of the
 * process; \c exists allows you to check whether a specific video room
 * exists; finally, \c list lists all the available rooms, while \c
 * listparticipants lists all the active (as in currentòy publishing
 * something) participants of a specific room and their details.
 *
 * The \c join , \c joinandconfigure , \c configure , \c publish ,
 * \c unpublish , \c start , \c pause , \c switch and \c leave
 * requests instead are all asynchronous, which
 * means you'll get a notification about their success or failure in
 * an event. \c join allows you to join a specific video room, specifying
 * whether that specific PeerConnection will be used for publishing or
 * watching; \c configure can be used to modify some of the participation
 * settings (e.g., bitrate cap); \c joinandconfigure combines the previous
 * two requests in a single one (just for publishers); \c publish can be
 * used to start sending media to broadcast to the other participants,
 * while \c unpublish does the opposite; \c start allows you to start
 * receiving media from a publisher you've subscribed to previously by
 * means of a \c join , while \c pause pauses the delivery of the media;
 * the \c switch request can be used to change the source of the media
 * flowing over a specific PeerConnection (e.g., I was watching Alice,
 * I want to watch Bob now) without having to create a new handle for
 * that; \c finally, \c leave allows you to leave a video room for good
 * (or, in the case of viewers, definitely closes a subscription).
 *
 * \c create can be used to create a new video room, and has to be
 * formatted as follows:
 *
\verbatim
{
	"request" : "create",
	"room" : <unique numeric ID, optional, chosen by plugin if missing>,
	"permanent" : <true|false, whether the room should be saved in the config file, default false>,
	"description" : "<pretty name of the room, optional>",
	"secret" : "<password required to edit/destroy the room, optional>",
	"pin" : "<password required to join the room, optional>",
	"is_private" : <true|false, whether the room should appear in a list request>,
	"allowed" : [ array of string tokens users can use to join this room, optional],
	...
}
\endverbatim
 *
 * For the sake of brevity, not all of the available settings are listed
 * here. You can refer to the name of the properties in the configuration
 * file as a reference, as the ones used to programmatically create a new
 * room are exactly the same.
 *
 * A successful creation procedure will result in a \c created response:
 *
\verbatim
{
	"videoroom" : "created",
	"room" : <unique numeric ID>,
	"permanent" : <true if saved to config file, false if not>
}
\endverbatim
 *
 * If you requested a permanent room but a \c false value is returned
 * instead, good chances are that there are permission problems.
 *
 * An error instead (and the same applies to all other requests, so this
 * won't be repeated) would provide both an error code and a more verbose
 * description of the cause of the issue:
 *
\verbatim
{
	"videoroom" : "event",
	"error_code" : <numeric ID, check Macros below>,
	"error" : "<error description as a string>"
}
\endverbatim
 *
 * Notice that, in general, all users can create rooms. If you want to
 * limit this functionality, you can configure an admin \c admin_key in
 * the plugin settings. When configured, only "create" requests that
 * include the correct \c admin_key value in an "admin_key" property
 * will succeed, and will be rejected otherwise.
 *
 * Once a room has been created, you can still edit some (but not all)
 * of its properties using the \c edit request. This allows you to modify
 * the room description, secret, pin and whether it's private or not: you
 * won't be able to modify other more static properties, like the room ID,
 * the sampling rate, the extensions-related stuff and so on. If you're
 * interested in changing the ACL, instead, check the \c allowed message.
 * An \c edit request has to be formatted as follows:
 *
\verbatim
{
	"request" : "edit",
	"room" : <unique numeric ID of the room to edit>,
	"secret" : "<room secret, mandatory if configured>",
	"new_description" : "<new pretty name of the room, optional>",
	"new_secret" : "<new password required to edit/destroy the room, optional>",
	"new_pin" : "<new password required to join the room, optional>",
	"new_is_private" : <true|false, whether the room should appear in a list request>,
	"new_require_pvtid" : <true|false, whether the room should require private_id from subscribers>,
	"new_bitrate" : <new bitrate cap to force on all publishers (except those with custom overrides)>,
	"new_fir_freq" : <new period for regular PLI keyframe requests to publishers>,
	"new_publishers" : <new cap on the number of concurrent active WebRTC publishers>,
	"permanent" : <true|false, whether the room should be also removed from the config file, default false>
}
\endverbatim
 *
 * A successful edit procedure will result in an \c edited response:
 *
\verbatim
{
	"videoroom" : "edited",
	"room" : <unique numeric ID>
}
\endverbatim
 *
 * On the other hand, \c destroy can be used to destroy an existing video
 * room, whether created dynamically or statically, and has to be
 * formatted as follows:
 *
\verbatim
{
	"request" : "destroy",
	"room" : <unique numeric ID of the room to destroy>,
	"secret" : "<room secret, mandatory if configured>",
	"permanent" : <true|false, whether the room should be also removed from the config file, default false>
}
\endverbatim
 *
 * A successful destruction procedure will result in a \c destroyed response:
 *
\verbatim
{
	"videoroom" : "destroyed",
	"room" : <unique numeric ID>
}
\endverbatim
 *
 * This will also result in a \c destroyed event being sent to all the
 * participants in the video room, which will look like this:
 *
\verbatim
{
	"videoroom" : "destroyed",
	"room" : <unique numeric ID of the destroyed room>
}
\endverbatim
 *
 * You can check whether a room exists using the \c exists request,
 * which has to be formatted as follows:
 *
\verbatim
{
	"request" : "exists",
	"room" : <unique numeric ID of the room to check>
}
\endverbatim
 *
 * A successful request will result in a \c success response:
 *
\verbatim
{
	"videoroom" : "success",
	"room" : <unique numeric ID>,
	"exists" : <true|false>
}
\endverbatim
 *
 * You can configure whether to check tokens or add/remove people who can join
 * a room using the \c allowed request, which has to be formatted as follows:
 *
\verbatim
{
	"request" : "allowed",
	"secret" : "<room secret, mandatory if configured>",
	"action" : "enable|disable|add|remove",
	"room" : <unique numeric ID of the room to update>,
	"allowed" : [
		// Array of strings (tokens users might pass in "join", only for add|remove)
	]
}
\endverbatim
 *
 * A successful request will result in a \c success response:
 *
\verbatim
{
	"videoroom" : "success",
	"room" : <unique numeric ID>,
	"allowed" : [
		// Updated, complete, list of allowed tokens (only for enable|add|remove)
	]
}
\endverbatim
 *
 * If you're the administrator of a room (that is, you created it and have access
 * to the secret) you can kick participants using the \c kick request. Notice
 * that this only kicks the user out of the room, but does not prevent them from
 * re-joining: to ban them, you need to first remove them from the list of
 * authorized users (see \c allowed request) and then \c kick them. The \c kick
 * request has to be formatted as follows:
 *
\verbatim
{
	"request" : "kick",
	"secret" : "<room secret, mandatory if configured>",
	"room" : <unique numeric ID of the room>,
	"id" : <unique numeric ID of the participant to kick>
}
\endverbatim
 *
 * A successful request will result in a \c success response:
 *
\verbatim
{
	"videoroom" : "success",
}
\endverbatim
 *
 * To get a list of the available rooms (excluded those configured or
 * created as private rooms) you can make use of the \c list request,
 * which has to be formatted as follows:
 *
\verbatim
{
	"request" : "list"
}
\endverbatim
 *
 * A successful request will produce a list of rooms in a \c success response:
 *
\verbatim
{
	"videoroom" : "success",
	"rooms" : [		// Array of room objects
		{	// Room #1
			"room" : <unique numeric ID>,
			"description" : "<Name of the room>",
			"pin_required" : <true|false, whether a PIN is required to join this room>,
			"max_publishers" : <how many publishers can actually publish via WebRTC at the same time>,
			"bitrate" : <bitrate cap that should be forced (via REMB) on all publishers by default>,
			"fir_freq" : <how often a keyframe request is sent via PLI/FIR to active publishers>,
			"audiocodec" : "<comma separated list of allowed audio codecs>",
			"videocodec" : "<comma separated list of allowed video codecs>",
			"record" : <true|false, whether the room is being recorded>,
			"record_dir" : "<if recording, the path where the .mjr files are being saved>",
			"num_participants" : <count of the participants (publishers, active or not; not subscribers)>
		},
		// Other rooms
	]
}
\endverbatim
 *
 * To get a list of the participants in a specific room, instead, you
 * can make use of the \c listparticipants request, which has to be
 * formatted as follows:
 *
\verbatim
{
	"request" : "listparticipants",
	"room" : <unique numeric ID of the room>
}
\endverbatim
 *
 * A successful request will produce a list of participants in a
 * \c participants response:
 *
\verbatim
{
	"videoroom" : "participants",
	"room" : <unique numeric ID of the room>,
	"participants" : [		// Array of participant objects
		{	// Participant #1
			"id" : <unique numeric ID of the participant>,
			"display" : "<display name of the participant, if any; optional>",
			"talking" : <true|false, whether user is talking or not (only if audio levels are used)>,
			"internal_audio_ssrc" : <audio SSRC used internally for this active publisher>,
			"internal_video_ssrc" : <video SSRC used internally for this active publisher>
		},
		// Other participants
	]
}
\endverbatim
 *
 * This covers almost all the synchronous requests. All the asynchronous requests,
 * plus a couple of additional synchronous requests we'll cover later, refer
 * to participants instead, namely on how they can publish, subscribe, or
 * more in general manage the media streams they may be sending or receiving.
 *
 * Considering the different nature of publishers and subscribers in the room,
 * and more importantly how you establish PeerConnections in the respective
 * cases, their API requests are addressed in separate subsections.
 *
 * \subsection vroompub VideoRoom Publishers
 *
 * In a VideoRoom, publishers are those participant handles that are able
 * (although may choose not to, more on this later) publish media in the
 * room, and as such become feeds that you can subscribe to.
 *
 * To specify a handle will be associated with a publisher, you must use
 * the \c join request with \c ptype set to \c publisher (note that, as it
 * will be explained later, you can also use \c joinandconfigure for the
 * purpose). The exact syntax of the request is the following:
 *
\verbatim
{
	"request" : "join",
	"ptype" : "publisher",
	"room" : <unique ID of the room to join>,
	"id" : <unique ID to register for the publisher; optional, will be chosen by the plugin if missing>,
	"display" : "<display name for the publisher; optional>",
	"token" : "<invitation token, in case the room has an ACL; optional>"
}
\endverbatim
 *
 * This will add the user to the list of participants in the room, although
 * in a non-active role for the time being. Anyway, this participation
 * allows the user to receive notifications about several aspects of the
 * room on the related handle (including streams as they become available
 * and go away). As such, it can be used even just as a way to get
 * notifications in a room, without the need of ever actually publishing
 * any stream at all (which explains why the "publisher" role may actually
 * be a bit confusing in this context). 
 *
 * A successful \c join will result in a \c joined event, which will contain
 * a list of the currently active (as in publishing via WebRTC) publishers:
 *
\verbatim
{
	"videoroom" : "joined",
	"room" : <room ID>,
	"description" : <description of the room, if available>,
	"id" : <unique ID of the participant>,
	"private_id" : <a different unique ID associated to the participant; meant to be private>,
	"publishers" : [
		{
			"id" : <unique ID of active publisher #1>,
			"display" : "<display name of active publisher #1, if any>",
			"audio_codec" : "<audio codec used by active publisher #1, if any>",
			"video_codec" : "<video codec used by active publisher #1, if any>",
			"talking" : <true|false, whether the publisher is talking or not (only if audio levels are used)>,
		},
		// Other active publishers
	]
}
\endverbatim
 *
 * Notice that the publishers list will of course be empty if no one is
 * currently active in the room. For what concerns the \c private_id
 * property, it is meant to be used by the user when they create subscriptions,
 * so that the plugin can associate subscriber handles (which are typically
 * anonymous) to a specific participant; they're usually optional, unless
 * required by the room configuration.
 *
 * As explained, with a simple \c join you're not an active publisher (there
 * is no WebRTC PeerConnection yet), which means that by default your presence
 * is not notified to other participants. In fact, the publish/subscribe nature
 * of the plugin implies that by default only active publishers are notified,
 * to allow participants to subscribe to existing feeds: notifying all joins/leaves,
 * even those related to who will just lurk, may be overly verbose and chatty,
 * especially in large rooms. Anyway, rooms can be configured to notify those
 * as well, if the \c notify_joining property is set to true: in that case,
 * regular joins will be notified too, in an event formatted like this:
 *
\verbatim
{
	"videoroom" : "event",
	"room" : <room ID>,
	"joining" : {
		"id" : <unique ID of the new participant>,
		"display" : "<display name of the new participant, if any>"
	}
}
\endverbatim
 *
 * If you're interested in publishing media within a room, you can do that
 * with a \c publish request. This request MUST be accompanied by a JSEP
 * SDP offer to negotiate a new PeerConnection. The plugin will match it
 * to the room configuration (e.g., to make sure the codecs you negotiated
 * are allowed in the room), and will reply with a JSEP SDP answer to
 * close the circle and complete the setup of the PeerConnection. As soon
 * as the PeerConnection has been establisher, the publisher will become
 * active, and a new active feed other participants can subscribe to.
 *
 * The syntax of a \c publish request is the following:
 *
\verbatim
{
	"request" : "publish",
	"audio" : <true|false, depending on whether or not audio should be relayed; true by default>,
	"video" : <true|false, depending on whether or not video should be relayed; true by default>,
	"data" : <true|false, depending on whether or not data should be relayed; true by default>,
	"audiocodec" : "<audio codec to prefer among the negotiated ones; optional>",
	"videocodec" : "<video codec to prefer among the negotiated ones; optional>",
	"bitrate" : <bitrate cap to return via REMB; optional, overrides the global room value if present>,
	"record" : <true|false, whether this publisher should be recorded or not; optional>,
	"filename" : "<if recording, the base path/file to use for the recording files; optional>",
	"display" : "<new display name to use in the room; optional>"
}
\endverbatim
 *
 * As anticipated, since this is supposed to be accompanied by a JSEP SDP
 * offer describing the publisher's media streams, the plugin will negotiate
 * and prepare a matching JSEP SDP answer. If successful, a \c configured
 * event will be sent back, formatted like this:
 *
\verbatim
{
	"videoroom" : "event",
	"configured" : "ok"
}
\endverbatim
 * 
 * This event will be accompanied by the prepared JSEP SDP answer.
 *
 * Notice that you can also use \c configure as a request instead of
 * \c publish to start publishing. The two are functionally equivalent
 * for publishing, but from a semantic perspective \c publish is the
 * right message to send when publishing. The \c configure request, as
 * it will be clearer later, can also be used to update some properties
 * of the publisher session: in this case the \c publish request can NOT
 * be used, as it can only be invoked to publish, and will fail if you're
 * already publishing something.
 *
 * As an additional note, notice that you can also join and publish in
 * a single request, which is useful in case you're not interested in
 * first join as a passive attendee and only later publish something,
 * but want to publish something right away. In this case you can use
 * the \c joinandconfigure request, which as you can imagine combines
 * the properties of both \c join and \c publish in a single request:
 * the response to a \c joinandconfigure will be a \c joined event, and
 * will again be accompanied by a JSEP SDP answer as usual.
 *
 * However you decided to publish something, as soon as the PeerConnection
 * setup succeeds and the publisher becomes active, an event is sent to
 * all the participants in the room with information on the new feed.
 * The event must contain an array with a single element, and be formatted like this:
 *
\verbatim
{
	"videoroom" : "event",
	"room" : <room ID>,
	"publishers" : [
		{
			"id" : <unique ID of the new publisher>,
			"display" : "<display name of the new publisher, if any>",
			"audio_codec" : "<audio codec used the new publisher, if any>",
			"video_codec" : "<video codec used by the new publisher, if any>",
			"talking" : <true|false, whether the publisher is talking or not (only if audio levels are used)>,
		}
	]
}
\endverbatim
 * 
 * To stop publishing and tear down the related PeerConnection, you can
 * use the \c unpublish request, which requires no arguments as the context
 * is implicit:
 *
\verbatim
{
	"request" : "unpublish"
}
\endverbatim
 *
 * This will have the plugin tear down the PeerConnection, and remove the
 * publisher from the list of active streams. If successful, the response
 * will look like this:
 *
\verbatim
{
	"videoroom" : "event",
	"unpublished" : "ok"
}
\endverbatim
 *
 * As soon as the PeerConnection is gone, all the other participants will
 * also be notified about the fact that the stream is no longer available: 
 *
\verbatim
{
	"videoroom" : "event",
	"room" : <room ID>,
	"unpublished" : <unique ID of the publisher who unpublished>
}
\endverbatim
 *
 * Notice that the same event will also be sent whenever the publisher
 * feed disappears for reasons other than an explicit \c unpublish , e.g.,
 * because the handle was closed or the user lost their connection.
 * Besides, notice that you can publish and unpublish multiple times
 * within the context of the same publisher handle.
 *
 * As anticipated above, you can use a request called \c configure to
 * tweak some of the properties of an active publisher session. This
 * request must be formatted as follows:
 *
\verbatim
{
	"request" : "configure",
	"audio" : <true|false, depending on whether or not audio should be relayed; true by default>,
	"video" : <true|false, depending on whether or not video should be relayed; true by default>,
	"data" : <true|false, depending on whether or not data should be relayed; true by default>,
	"bitrate" : <bitrate cap to return via REMB; optional, overrides the global room value if present>,
	"record" : <true|false, whether this publisher should be recorded or not; optional>,
	"filename" : "<if recording, the base path/file to use for the recording files; optional>",
	"display" : "<new display name to use in the room; optional>"
}
\endverbatim
 *
 * As you can see, it's basically the same properties as those listed for
 * \c publish . This is why both requests can be used to start publishing,
 * as even in that case you configure some of the settings. If successful,
 * a \c configured event will be sent back as before, formatted like this:
 *
\verbatim
{
	"videoroom" : "event",
	"configured" : "ok"
}
\endverbatim
 *
 * An interesting feature VideoRoom publisher can take advantage of is
 * RTP forwarding. In fact, while the main purpose of this plugin is
 * getting media from WebRTC sources (publishers) and relaying it to
 * WebRTC destinations (subscribers), there are actually several use
 * cases and scenarios for making this media available to external,
 * notnecessarily WebRTC-compliant, components. These components may
 * benefit from having access to the RTP media sent by a publisher, e.g.,
 * for media processing, external recording, transcoding to other
 * technologies via other applications, scalability purposes or
 * whatever else makes sense in this context. This is made possible by
 * a request called \c rtp_forward which, as the name suggests, simply
 * forwards in real-time the media sent by a publisher via RTP (plain
 * or encrypted) to a remote backend.
 *
 * You can add a new RTP forwarder for an existing publisher using the
 * \c rtp_forward request, which has to be formatted as follows:
 *
\verbatim
{
	"request" : "rtp_forward",
	"room" : <unique numeric ID of the room the publisher is in>,
	"publisher_id" : <unique numeric ID of the publisher to relay externally>,
	"host" : "<host address to forward the RTP and data packets to>",
	"audio_port" : <port to forward the audio RTP packets to>,
	"audio_ssrc" : <audio SSRC to use to use when streaming; optional>,
	"audio_ptype" : <audio payload type to use when streaming; optional>,
	"video_port" : <port to forward the video RTP packets to>,
	"video_ssrc" : <video SSRC to use to use when streaming; optional>,
	"video_ptype" : <video payload type to use when streaming; optional>,
	"video_port_2" : <if simulcasting or doing VP9-SVC, port to forward the video RTP packets from the second substream/layer to>,
	"video_ssrc_2" : <if simulcasting or doing VP9-SVC, video SSRC to use to use the second substream/layer; optional>,
	"video_ptype_2" : <if simulcasting or doing VP9-SVC, video payload type to use the second substream/layer; optional>,
	"video_port_3" : <if simulcasting or doing VP9-SVC, port to forward the video RTP packets from the third substream/layer to>,
	"video_ssrc_3" : <if simulcasting or doing VP9-SVC, video SSRC to use to use the third substream/layer; optional>,
	"video_ptype_3" : <if simulcasting or doing VP9-SVC, video payload type to use the third substream/layer; optional>,
	"data_port" : <port to forward the datachannel messages to>,
	"srtp_suite" : <length of authentication tag (32 or 80); optional>,
	"srtp_crypto" : "<key to use as crypto (base64 encoded key as in SDES); optional>"
}
\endverbatim
 *
 * A successful request will result in an \c rtp_forward response, containing
 * the relevant info associated to the new forwarder(s):
 *
\verbatim
{
	"videoroom" : "rtp_forward",
	"room" : <unique numeric ID, same as request>,
	"publisher_id" : <unique numeric ID, same as request>,
	"rtp_stream" : {
		"host" : "<host this forwarder is streaming to, same as request>",
		"audio" : <audio port, same as request if configured>,
		"audio_stream_id" : <unique numeric ID assigned to the audio RTP forwarder, if any>,
		"video" : <video port, same as request if configured>,
		"video_stream_id" : <unique numeric ID assigned to the main video RTP forwarder, if any>,
		"video_2" : <second video port, same as request if configured>,
		"video_stream_id_2" : <unique numeric ID assigned to the second video RTP forwarder, if any>,
		"video_3" : <third video port, same as request if configured>,
		"video_stream_id_3" : <unique numeric ID assigned to the third video RTP forwarder, if any>,
		"data" : <data port, same as request if configured>,
		"data_stream_id" : <unique numeric ID assigned to datachannel messages forwarder, if any>
	}
}
\endverbatim
 *
 * To stop a previously created RTP forwarder and stop it, you can use
 * the \c stop_rtp_forward request, which has to be formatted as follows:
 *
\verbatim
{
	"request" : "stop_rtp_forward",
	"room" : <unique numeric ID of the room the publisher is in>,
	"publisher_id" : <unique numeric ID of the publisher to update>,
	"stream_id" : <unique numeric ID of the RTP forwarder>
}
\endverbatim
 *
 * A successful request will result in a \c stop_rtp_forward response:
 *
\verbatim
{
	"videoroom" : "stop_rtp_forward",
	"room" : <unique numeric ID, same as request>,
	"publisher_id" : <unique numeric ID, same as request>,
	"stream_id" : <unique numeric ID, same as request>
}
\endverbatim
 *
 * To get a list of all the forwarders in a specific room, instead, you
 * can make use of the \c listforwarders request, which has to be
 * formatted as follows:
 *
\verbatim
{
	"request" : "listforwarders",
	"room" : <unique numeric ID of the room>,
	"secret" : "<room secret; mandatory if configured>"
}
\endverbatim
 *
 * A successful request will produce a list of RTP forwarders in a
 * \c forwarders response:
 *
\verbatim
{
	"videoroom" : "forwarders",
	"room" : <unique numeric ID of the room>,
	"rtp_forwarders" : [		// Array of publishers with RTP forwarders
		{	// Publisher #1
			"publisher_id" : <unique numeric ID of publisher #1>,
			"rtp_forwarders" : [		// Array of RTP forwarders
				{	// RTP forwarder #1
					"audio_stream_id" : <unique numeric ID assigned to this audio RTP forwarder, if any>,
					"video_stream_id" : <unique numeric ID assigned to this video RTP forwarder, if any>,
					"data_stream_id" : <unique numeric ID assigned to this datachannel messages forwarder, if any>
					"ip" : "<IP this forwarder is streaming to>",
					"port" : <port this forwarder is streaming to>,
					"ssrc" : <SSRC this forwarder is using, if any>,
					"pt" : <payload type this forwarder is using, if any>,
					"substream" : <video substream this video forwarder is relaying, if any>,
					"srtp" : <true|false, whether the RTP stream is encrypted>
				},
				// Other forwarders for this publisher
			],
		},
		// Other publishers
	]
}
\endverbatim * 
 * 
 * To conclude, you can leave a room you previously joined as publisher
 * using the \c leave request. This will also implicitly unpublish you
 * if you were an active publisher in the room. The \c leave request
 * looks like follows:
 *
\verbatim
{
	"request" : "leave"
}
\endverbatim
 *
 * If successful, the response will look like this:
 *
\verbatim
{
	"videoroom" : "event",
	"leaving" : "ok"
}
\endverbatim
 *
 * Other participants will receive a different event depending on whether
 * you were currently an active publisher ("unpublished") or simply
 * lurking ("leaving"):
 *
\verbatim
{
	"videoroom" : "event",
	"room" : <room ID>,
	"leaving|unpublished" : <unique ID of the publisher who left>
}
\endverbatim
 *
 * \subsection vroomsub VideoRoom Subscribers
 *
 * In a VideoRoom, subscribers are NOT participants, but simply handles
 * that will be used exclusively to receive media from a specific publisher
 * in the room. Since they're not participants per se, they're basically
 * streams that can be (and typically are) associated to publisher handles
 * as the ones we introduced in the previous section, whether active or not.
 * In fact, the typical use case is publishers being notified about new
 * participants becoming active in the room, and as a result new subscriber
 * sessions being created to receive their media streams; as soon as the
 * publisher goes away, the subscriber handle is removed as well. As such,
 * these subscriber sessions are dependent on feedback obtained by
 * publishers, and can't exist on their own, unless you feed them the
 * right info out of band.
 *
 * To specify a handle will be associated with a subscriber, you must use
 * the \c join request with \c ptype set to \c subscriber and specify which
 * feed to subscribe to. The exact syntax of the request is the following:
 *
\verbatim
{
	"request" : "join",
	"ptype" : "subscriber",
	"room" : <unique ID of the room to subscribe in>,
	"feed" : <unique ID of the publisher to subscribe to; mandatory>,
	"private_id" : <unique ID of the publisher that originated this request; optional, unless mandated by the room configuration>,
	"close_pc" : <true|false, depending on whether or not the PeerConnection should be automatically closed when the publisher leaves; true by default>,
	"audio" : <true|false, depending on whether or not audio should be relayed; true by default>,
	"video" : <true|false, depending on whether or not video should be relayed; true by default>,
	"data" : <true|false, depending on whether or not data should be relayed; true by default>,
	"offer_audio" : <true|false; whether or not audio should be negotiated; true by default if the publisher has audio>,
	"offer_video" : <true|false; whether or not video should be negotiated; true by default if the publisher has video>,
	"offer_data" : <true|false; whether or not datachannels should be negotiated; true by default if the publisher has datachannels>
}
\endverbatim
 *
 * As you can see, it's just a matter of specifying the ID of the publisher to
 * subscribe to and, if needed, your own \c private_id (if mandated by the room).
 * The \c offer_audio , \c offer_video and \c offer_data are
 * also particularly interesting, though, as they allow you to only subscribe
 * to a subset of the mountpoint media. By default, in fact, this \c join
 * request will result in the plugin preparing a new SDP offer trying to
 * negotiate all the media streams made available by the publisher; in case
 * the subscriber knows they don't support one of the mountpoint codecs, though
 * (e.g., the video in the mountpoint is VP8, but they only support H.264),
 * or are not interested in getting all the media (e.g., they're ok with
 * just audio and not video, or don't have enough bandwidth for both),
 * they can use those properties to shape the SDP offer to their needs.
 *
 * As anticipated, if successful this request will generate a new JSEP SDP
 * offer, which will accompany an \c attached event:
 *
\verbatim
{
	"videoroom" : "attached",
	"room" : <room ID>,
	"feed" : <publisher ID>,
	"display" : "<the display name of the publisher, if any>"
}
\endverbatim
 *
 * At this stage, to complete the setup of the PeerConnection the subscriber is
 * supposed to send a JSEP SDP answer back to the plugin. This is done
 * by means of a \c start request, which in this case MUST be associated
 * with a JSEP SDP answer but otherwise requires no arguments:
 *
\verbatim
{
	"request" : "start"
}
\endverbatim
 *
 * If successful this request returns a \c started event:
 *
\verbatim
{
	"videoroom" : "event",
	"started" : "ok"
}
\endverbatim
 *
 * Once this is done, all that's needed is waiting for the WebRTC PeerConnection
 * establishment to succeed. As soon as that happens, the Streaming plugin
 * can start relaying media from the mountpoint the viewer subscribed to
 * to the viewer themselves.
 *
 * Notice that the same exact steps we just went through (\c watch request,
 * followed by JSEP offer by the plugin, followed by \c start request with
 * JSEP answer by the viewer) is what you also use when renegotiations are
 * needed, e.g., for the purpose of ICE restarts.
 *
 * As a subscriber, you can temporarily pause and resume the whole media delivery
 * with a \c pause and, again, \c start request (in this case without any JSEP
 * SDP answer attached). Neither expect other arguments, as the context
 * is implicitly derived from the handle they're sent on:
 *
\verbatim
{
	"request" : "pause"
}
\endverbatim
 *
\verbatim
{
	"request" : "start"
}
\endverbatim
 *
 * Unsurprisingly, they just result in, respectively, \c paused and
 * \c started events:
 *
\verbatim
{
	"videoroom" : "event",
	"paused" : "ok"
}
\endverbatim
 *
\verbatim
{
	"videoroom" : "event",
	"started" : "ok"
}
\endverbatim
 *
 * For more drill-down manipulations of a subscription, a \c configure
 * request can be used instead. This request allows subscribers to dynamically
 * change some properties associated to their media subscription, e.g.,
 * in terms of what should and should not be sent at a specific time. A
 * \c configure request must be formatted as follows:
 *
\verbatim
{
	"request" : "configure",
	"audio" : <true|false, depending on whether audio should be relayed or not; optional>,
	"video" : <true|false, depending on whether video should be relayed or not; optional>,
	"data" : <true|false, depending on whether datachannel messages should be relayed or not; optional>,
	"substream" : <substream to receive (0-2), in case simulcasting is enabled; optional>,
	"temporal" : <temporal layers to receive (0-2), in case simulcasting is enabled; optional>,
	"spatial_layer" : <spatial layer to receive (0-1), in case VP9-SVC is enabled; optional>,
	"temporal_layer" : <temporal layers to receive (0-2), in case VP9-SVC is enabled; optional>
}
\endverbatim
 *
 * As you can see, the \c audio , \c video and \c data properties can be
 * used as a media-level pause/resume functionality, whereas \c pause
 * and \c start simply pause and resume all streams at the same time.
 * The \c substream and \c temporal properties, instead, only make sense
 * when the mountpoint is configured with video simulcasting support, and
 * as such the viewer is interested in receiving a specific substream
 * or temporal layer, rather than any other of the available ones.
 * The \c spatial_layer and \c temporal_layer have exactly the same meaning,
 * but within the context of VP9-SVC publishers, and will have no effect
 * on subscriptions associated to regular publishers.
 *
 * Another interesting feature that subscribers can take advantage of is the
 * so-called publisher "switching". Basically, when subscribed to a specific
 * publisher and receiving media from them, you can at any time "switch"
 * to a different publisher, and as such start receiving media from that
 * other mountpoint instead. Think of it as changing channel on a TV: you
 * keep on using the same PeerConnection, the plugin simply changes the
 * source of the media transparently. Of course, while powerful and effective
 * this request has some limitations. First of all, it switches both audio
 * and video, meaning you can't just switch video and keep the audio from
 * the previous publisher, for instance; besides, the two publishers
 * must have the same media configuration, that is, use the same codecs,
 * the same payload types, etc. In fact, since the same PeerConnection is
 * used for this feature, switching to a publisher with a different
 * configuration might result in media incompatible with the PeerConnection
 * setup being relayed to the subscriber, and as such in no audio/video being
 * played. That said, a \c switch request must be formatted like this:
 *
\verbatim
{
	"request" : "switch",
	"feed" : <unique ID of the new publisher to switch to; mandatory>,
	"audio" : <true|false, depending on whether audio should be relayed or not; optional>,
	"video" : <true|false, depending on whether video should be relayed or not; optional>,
	"data" : <true|false, depending on whether datachannel messages should be relayed or not; optional>
}
\endverbatim
 *
 * If successful, you'll be unsubscribed from the previous publisher,
 * and subscribed to the new publisher instead. The event to confirm
 * the switch was successful will look like this:
 *
\verbatim
{
	"videoroom" : "event",
	"switched" : "ok",
	"room" : <room ID>,
	"id" : <unique ID of the new publisher>
}
\endverbatim
 *
 * Finally, to stop the subscription to the mountpoint and tear down the
 * related PeerConnection, you can use the \c leave request. Since context
 * is implicit, no other argument is required:
 *
\verbatim
{
	"request" : "leave"
}
\endverbatim
 *
 * If successful, the plugin will attempt to tear down the PeerConnection,
 * and will send back a \c left event:
 *
\verbatim
{
	"videoroom" : "event",
	"left" : "ok",
}
\endverbatim
 *
 *
 *
 *
 *
 *
 * \ingroup plugins
 * \ref plugins
 */

#include "plugin.h"

#include <jansson.h>

#include "../debug.h"
#include "../apierror.h"
#include "../config.h"
#include "../mutex.h"
#include "../rtp.h"
#include "../rtpsrtp.h"
#include "../rtcp.h"
#include "../record.h"
#include "../sdp-utils.h"
#include "../utils.h"
#include <sys/types.h>
#include <sys/socket.h>


/* Plugin information */
#define JANUS_VIDEOROOM_VERSION			9
#define JANUS_VIDEOROOM_VERSION_STRING	"0.0.9"
#define JANUS_VIDEOROOM_DESCRIPTION		"This is a plugin implementing a videoconferencing SFU (Selective Forwarding Unit) for Janus, that is an audio/video router."
#define JANUS_VIDEOROOM_NAME			"JANUS VideoRoom plugin"
#define JANUS_VIDEOROOM_AUTHOR			"Meetecho s.r.l."
#define JANUS_VIDEOROOM_PACKAGE			"janus.plugin.videoroom"

/* Plugin methods */
janus_plugin *create(void);
int janus_videoroom_init(janus_callbacks *callback, const char *config_path);
void janus_videoroom_destroy(void);
int janus_videoroom_get_api_compatibility(void);
int janus_videoroom_get_version(void);
const char *janus_videoroom_get_version_string(void);
const char *janus_videoroom_get_description(void);
const char *janus_videoroom_get_name(void);
const char *janus_videoroom_get_author(void);
const char *janus_videoroom_get_package(void);
void janus_videoroom_create_session(janus_plugin_session *handle, int *error);
struct janus_plugin_result *janus_videoroom_handle_message(janus_plugin_session *handle, char *transaction, json_t *message, json_t *jsep);
void janus_videoroom_setup_media(janus_plugin_session *handle);
void janus_videoroom_incoming_rtp(janus_plugin_session *handle, int video, char *buf, int len);
void janus_videoroom_incoming_rtcp(janus_plugin_session *handle, int video, char *buf, int len);
void janus_videoroom_incoming_data(janus_plugin_session *handle, char *buf, int len);
void janus_videoroom_slow_link(janus_plugin_session *handle, int uplink, int video);
void janus_videoroom_hangup_media(janus_plugin_session *handle);
void janus_videoroom_destroy_session(janus_plugin_session *handle, int *error);
json_t *janus_videoroom_query_session(janus_plugin_session *handle);

/* Plugin setup */
static janus_plugin janus_videoroom_plugin =
	JANUS_PLUGIN_INIT (
		.init = janus_videoroom_init,
		.destroy = janus_videoroom_destroy,

		.get_api_compatibility = janus_videoroom_get_api_compatibility,
		.get_version = janus_videoroom_get_version,
		.get_version_string = janus_videoroom_get_version_string,
		.get_description = janus_videoroom_get_description,
		.get_name = janus_videoroom_get_name,
		.get_author = janus_videoroom_get_author,
		.get_package = janus_videoroom_get_package,
		
		.create_session = janus_videoroom_create_session,
		.handle_message = janus_videoroom_handle_message,
		.setup_media = janus_videoroom_setup_media,
		.incoming_rtp = janus_videoroom_incoming_rtp,
		.incoming_rtcp = janus_videoroom_incoming_rtcp,
		.incoming_data = janus_videoroom_incoming_data,
		.slow_link = janus_videoroom_slow_link,
		.hangup_media = janus_videoroom_hangup_media,
		.destroy_session = janus_videoroom_destroy_session,
		.query_session = janus_videoroom_query_session,
	);

/* Plugin creator */
janus_plugin *create(void) {
	JANUS_LOG(LOG_VERB, "%s created!\n", JANUS_VIDEOROOM_NAME);
	return &janus_videoroom_plugin;
}

/* Parameter validation */
static struct janus_json_parameter request_parameters[] = {
	{"request", JSON_STRING, JANUS_JSON_PARAM_REQUIRED}
};
static struct janus_json_parameter adminkey_parameters[] = {
	{"admin_key", JSON_STRING, JANUS_JSON_PARAM_REQUIRED}
};
static struct janus_json_parameter create_parameters[] = {
	{"room", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"description", JSON_STRING, 0},
	{"is_private", JANUS_JSON_BOOL, 0},
	{"allowed", JSON_ARRAY, 0},
	{"secret", JSON_STRING, 0},
	{"pin", JSON_STRING, 0},
	{"require_pvtid", JANUS_JSON_BOOL, 0},
	{"bitrate", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"fir_freq", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"publishers", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"audiocodec", JSON_STRING, 0},
	{"videocodec", JSON_STRING, 0},
	{"video_svc", JANUS_JSON_BOOL, 0},
	{"audiolevel_ext", JANUS_JSON_BOOL, 0},
	{"audiolevel_event", JANUS_JSON_BOOL, 0},
	{"audio_active_packets", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"audio_level_average", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"videoorient_ext", JANUS_JSON_BOOL, 0},
	{"playoutdelay_ext", JANUS_JSON_BOOL, 0},
	{"transport_wide_cc_ext", JANUS_JSON_BOOL, 0},
	{"record", JANUS_JSON_BOOL, 0},
	{"rec_dir", JSON_STRING, 0},
	{"permanent", JANUS_JSON_BOOL, 0},
	{"notify_joining", JANUS_JSON_BOOL, 0},
};
static struct janus_json_parameter edit_parameters[] = {
	{"room", JSON_INTEGER, JANUS_JSON_PARAM_REQUIRED | JANUS_JSON_PARAM_POSITIVE},
	{"secret", JSON_STRING, 0},
	{"new_description", JSON_STRING, 0},
	{"new_is_private", JANUS_JSON_BOOL, 0},
	{"new_secret", JSON_STRING, 0},
	{"new_pin", JSON_STRING, 0},
	{"new_require_pvtid", JANUS_JSON_BOOL, 0},
	{"new_bitrate", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"new_fir_freq", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"new_publishers", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"permanent", JANUS_JSON_BOOL, 0}
};
static struct janus_json_parameter room_parameters[] = {
	{"room", JSON_INTEGER, JANUS_JSON_PARAM_REQUIRED | JANUS_JSON_PARAM_POSITIVE}
};
static struct janus_json_parameter destroy_parameters[] = {
	{"room", JSON_INTEGER, JANUS_JSON_PARAM_REQUIRED | JANUS_JSON_PARAM_POSITIVE},
	{"permanent", JANUS_JSON_BOOL, 0}
};
static struct janus_json_parameter allowed_parameters[] = {
	{"room", JSON_INTEGER, JANUS_JSON_PARAM_REQUIRED | JANUS_JSON_PARAM_POSITIVE},
	{"secret", JSON_STRING, 0},
	{"action", JSON_STRING, JANUS_JSON_PARAM_REQUIRED},
	{"allowed", JSON_ARRAY, 0}
};
static struct janus_json_parameter kick_parameters[] = {
	{"room", JSON_INTEGER, JANUS_JSON_PARAM_REQUIRED | JANUS_JSON_PARAM_POSITIVE},
	{"secret", JSON_STRING, 0},
	{"id", JSON_INTEGER, JANUS_JSON_PARAM_REQUIRED | JANUS_JSON_PARAM_POSITIVE}
};
static struct janus_json_parameter join_parameters[] = {
	{"room", JSON_INTEGER, JANUS_JSON_PARAM_REQUIRED | JANUS_JSON_PARAM_POSITIVE},
	{"ptype", JSON_STRING, JANUS_JSON_PARAM_REQUIRED},
	{"audio", JANUS_JSON_BOOL, 0},
	{"video", JANUS_JSON_BOOL, 0},
	{"data", JANUS_JSON_BOOL, 0},
	{"bitrate", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"record", JANUS_JSON_BOOL, 0},
	{"filename", JSON_STRING, 0},
	{"token", JSON_STRING, 0}
};
static struct janus_json_parameter publish_parameters[] = {
	{"audio", JANUS_JSON_BOOL, 0},
	{"audiocodec", JSON_STRING, 0},
	{"video", JANUS_JSON_BOOL, 0},
	{"videocodec", JSON_STRING, 0},
	{"data", JANUS_JSON_BOOL, 0},
	{"bitrate", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"record", JANUS_JSON_BOOL, 0},
	{"filename", JSON_STRING, 0},
	{"display", JSON_STRING, 0},
	/* The following are just to force a renegotiation and/or an ICE restart */
	{"update", JANUS_JSON_BOOL, 0},
	{"restart", JANUS_JSON_BOOL, 0}
};
static struct janus_json_parameter rtp_forward_parameters[] = {
	{"room", JSON_INTEGER, JANUS_JSON_PARAM_REQUIRED | JANUS_JSON_PARAM_POSITIVE},
	{"publisher_id", JSON_INTEGER, JANUS_JSON_PARAM_REQUIRED | JANUS_JSON_PARAM_POSITIVE},
	{"video_port", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"video_ssrc", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"video_pt", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"video_port_2", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"video_ssrc_2", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"video_pt_2", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"video_port_3", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"video_ssrc_3", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"video_pt_3", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"audio_port", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"audio_ssrc", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"audio_pt", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"data_port", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"host", JSON_STRING, JANUS_JSON_PARAM_REQUIRED},
	{"srtp_suite", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"srtp_crypto", JSON_STRING, 0}
};
static struct janus_json_parameter stop_rtp_forward_parameters[] = {
	{"room", JSON_INTEGER, JANUS_JSON_PARAM_REQUIRED | JANUS_JSON_PARAM_POSITIVE},
	{"publisher_id", JSON_INTEGER, JANUS_JSON_PARAM_REQUIRED | JANUS_JSON_PARAM_POSITIVE},
	{"stream_id", JSON_INTEGER, JANUS_JSON_PARAM_REQUIRED | JANUS_JSON_PARAM_POSITIVE}
};
static struct janus_json_parameter publisher_parameters[] = {
	{"id", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"display", JSON_STRING, 0}
};
static struct janus_json_parameter configure_parameters[] = {
	{"audio", JANUS_JSON_BOOL, 0},
	{"video", JANUS_JSON_BOOL, 0},
	{"data", JANUS_JSON_BOOL, 0},
	/* For VP8 simulcast */
	{"substream", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"temporal", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	/* For VP9 SVC */
	{"spatial_layer", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"temporal_layer", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	/* The following is to handle a renegotiation */
	{"update", JANUS_JSON_BOOL, 0},
};
static struct janus_json_parameter subscriber_parameters[] = {
	{"feed", JSON_INTEGER, JANUS_JSON_PARAM_REQUIRED | JANUS_JSON_PARAM_POSITIVE},
	{"private_id", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"close_pc", JANUS_JSON_BOOL, 0},
	{"audio", JANUS_JSON_BOOL, 0},
	{"video", JANUS_JSON_BOOL, 0},
	{"data", JANUS_JSON_BOOL, 0},
	{"offer_audio", JANUS_JSON_BOOL, 0},
	{"offer_video", JANUS_JSON_BOOL, 0},
	{"offer_data", JANUS_JSON_BOOL, 0}
};

/* Static configuration instance */
static janus_config *config = NULL;
static const char *config_folder = NULL;
static janus_mutex config_mutex = JANUS_MUTEX_INITIALIZER;

/* Useful stuff */
static volatile gint initialized = 0, stopping = 0;
static gboolean notify_events = TRUE;
static janus_callbacks *gateway = NULL;
static GThread *handler_thread;
static void *janus_videoroom_handler(void *data);
static void janus_videoroom_relay_rtp_packet(gpointer data, gpointer user_data);
static void janus_videoroom_relay_data_packet(gpointer data, gpointer user_data);
static void janus_videoroom_hangup_media_internal(janus_plugin_session *handle);

typedef enum janus_videoroom_p_type {
	janus_videoroom_p_type_none = 0,
	janus_videoroom_p_type_subscriber,			/* Generic subscriber */
	janus_videoroom_p_type_publisher,			/* Participant (for receiving events) and optionally publisher */
} janus_videoroom_p_type;

typedef struct janus_videoroom_message {
	janus_plugin_session *handle;
	char *transaction;
	json_t *message;
	json_t *jsep;
} janus_videoroom_message;
static GAsyncQueue *messages = NULL;
static janus_videoroom_message exit_message;


/* Payload types we'll offer internally */
#define OPUS_PT		111
#define ISAC32_PT	104
#define ISAC16_PT	103
#define PCMU_PT		0
#define PCMA_PT		8
#define G722_PT		9
#define VP8_PT		96
#define VP9_PT		101
#define H264_PT		107

typedef enum janus_videoroom_audiocodec {
	JANUS_VIDEOROOM_NOAUDIO,	/* Publishers won't do any audio */
	JANUS_VIDEOROOM_OPUS,		/* Publishers will have to/may use OPUS */
	JANUS_VIDEOROOM_PCMU,		/* Publishers will have to/may use PCMU 8K */
	JANUS_VIDEOROOM_PCMA,		/* Publishers will have to/may use PCMA 8K */
	JANUS_VIDEOROOM_G722,		/* Publishers will have to/may use G.722 */
	JANUS_VIDEOROOM_ISAC_32K,	/* Publishers will have to/may use ISAC 32K */
	JANUS_VIDEOROOM_ISAC_16K	/* Publishers will have to/may use ISAC 16K */
} janus_videoroom_audiocodec;
static const char *janus_videoroom_audiocodec_name(janus_videoroom_audiocodec acodec) {
	switch(acodec) {
		case JANUS_VIDEOROOM_NOAUDIO:
			return "none";
		case JANUS_VIDEOROOM_OPUS:
			return "opus";
		case JANUS_VIDEOROOM_PCMU:
			return "pcmu";
		case JANUS_VIDEOROOM_PCMA:
			return "pcma";
		case JANUS_VIDEOROOM_G722:
			return "g722";
		case JANUS_VIDEOROOM_ISAC_32K:
			return "isac32";
		case JANUS_VIDEOROOM_ISAC_16K:
			return "isac16";
		default:
			/* Shouldn't happen */
			return "opus";
	}
}
static janus_videoroom_audiocodec janus_videoroom_audiocodec_from_name(const char *name) {
	if(name == NULL)
		return JANUS_VIDEOROOM_NOAUDIO;
	else if(!strcasecmp(name, "opus"))
		return JANUS_VIDEOROOM_OPUS;
	else if(!strcasecmp(name, "isac32"))
		return JANUS_VIDEOROOM_ISAC_32K;
	else if(!strcasecmp(name, "isac16"))
		return JANUS_VIDEOROOM_ISAC_16K;
	else if(!strcasecmp(name, "pcmu"))
		return JANUS_VIDEOROOM_PCMU;
	else if(!strcasecmp(name, "pcma"))
		return JANUS_VIDEOROOM_PCMA;
	else if(!strcasecmp(name, "g722"))
		return JANUS_VIDEOROOM_G722;
	JANUS_LOG(LOG_WARN, "Unsupported audio codec '%s'\n", name);
	return JANUS_VIDEOROOM_NOAUDIO;
}
static int janus_videoroom_audiocodec_pt(janus_videoroom_audiocodec acodec) {
	switch(acodec) {
		case JANUS_VIDEOROOM_NOAUDIO:
			return -1;
		case JANUS_VIDEOROOM_OPUS:
			return OPUS_PT;
		case JANUS_VIDEOROOM_ISAC_32K:
			return ISAC32_PT;
		case JANUS_VIDEOROOM_ISAC_16K:
			return ISAC16_PT;
		case JANUS_VIDEOROOM_PCMU:
			return PCMU_PT;
		case JANUS_VIDEOROOM_PCMA:
			return PCMA_PT;
		case JANUS_VIDEOROOM_G722:
			return G722_PT;
		default:
			/* Shouldn't happen */
			return OPUS_PT;
	}
}

typedef enum janus_videoroom_videocodec {
	JANUS_VIDEOROOM_NOVIDEO,	/* Publishers won't do any audio */
	JANUS_VIDEOROOM_VP8,		/* Publishers will have to/may use VP8 */
	JANUS_VIDEOROOM_VP9,		/* Publishers will have to/may use VP9 */
	JANUS_VIDEOROOM_H264		/* Publishers will have to/may use H264 */
} janus_videoroom_videocodec;
static const char *janus_videoroom_videocodec_name(janus_videoroom_videocodec vcodec) {
	switch(vcodec) {
		case JANUS_VIDEOROOM_NOVIDEO:
			return "none";
		case JANUS_VIDEOROOM_VP8:
			return "vp8";
		case JANUS_VIDEOROOM_VP9:
			return "vp9";
		case JANUS_VIDEOROOM_H264:
			return "h264";
		default:
			/* Shouldn't happen */
			return "vp8";
	}
}
static janus_videoroom_videocodec janus_videoroom_videocodec_from_name(const char *name) {
	if(name == NULL)
		return JANUS_VIDEOROOM_NOVIDEO;
	else if(!strcasecmp(name, "vp8"))
		return JANUS_VIDEOROOM_VP8;
	else if(!strcasecmp(name, "vp9"))
		return JANUS_VIDEOROOM_VP9;
	else if(!strcasecmp(name, "h264"))
		return JANUS_VIDEOROOM_H264;
	JANUS_LOG(LOG_WARN, "Unsupported video codec '%s'\n", name);
	return JANUS_VIDEOROOM_NOVIDEO;
}
static int janus_videoroom_videocodec_pt(janus_videoroom_videocodec vcodec) {
	switch(vcodec) {
		case JANUS_VIDEOROOM_NOVIDEO:
			return -1;
		case JANUS_VIDEOROOM_VP8:
			return VP8_PT;
		case JANUS_VIDEOROOM_VP9:
			return VP9_PT;
		case JANUS_VIDEOROOM_H264:
			return H264_PT;
		default:
			/* Shouldn't happen */
			return VP8_PT;
	}
}


typedef struct janus_videoroom {
	guint64 room_id;			/* Unique room ID */
	gchar *room_name;			/* Room description */
	gchar *room_secret;			/* Secret needed to manipulate (e.g., destroy) this room */
	gchar *room_pin;			/* Password needed to join this room, if any */
	gboolean is_private;		/* Whether this room is 'private' (as in hidden) or not */
	gboolean require_pvtid;		/* Whether subscriptions in this room require a private_id */
	int max_publishers;			/* Maximum number of concurrent publishers */
	uint32_t bitrate;			/* Global bitrate limit */
	uint16_t fir_freq;			/* Regular FIR frequency (0=disabled) */
	janus_videoroom_audiocodec acodec[3];	/* Audio codec(s) to force on publishers */
	janus_videoroom_videocodec vcodec[3];	/* Video codec(s) to force on publishers */
	gboolean do_svc;			/* Whether SVC must be done for video (note: only available for VP9 right now) */
	gboolean audiolevel_ext;	/* Whether the ssrc-audio-level extension must be negotiated or not for new publishers */
	gboolean audiolevel_event;	/* Whether to emit event to other users about audiolevel */
	int audio_active_packets;	/* Amount of packets with audio level for checkup */
	int audio_level_average;	/* Average audio level */
	gboolean videoorient_ext;	/* Whether the video-orientation extension must be negotiated or not for new publishers */
	gboolean playoutdelay_ext;	/* Whether the playout-delay extension must be negotiated or not for new publishers */
	gboolean transport_wide_cc_ext;	/* Whether the transport wide cc extension must be negotiated or not for new publishers */
	gboolean record;			/* Whether the feeds from publishers in this room should be recorded */
	char *rec_dir;				/* Where to save the recordings of this room, if enabled */
	GHashTable *participants;	/* Map of potential publishers (we get subscribers from them) */
	GHashTable *private_ids;	/* Map of existing private IDs */
	volatile gint destroyed;	/* Whether this room has been destroyed */
	gboolean check_allowed;		/* Whether to check tokens when participants join (see below) */
	GHashTable *allowed;		/* Map of participants (as tokens) allowed to join */
	gboolean notify_joining;	/* Whether an event is sent to notify all participants if a new participant joins the room */
	janus_mutex mutex;			/* Mutex to lock this room instance */
	janus_refcount ref;			/* Reference counter for this room */
} janus_videoroom;
static GHashTable *rooms;
static janus_mutex rooms_mutex = JANUS_MUTEX_INITIALIZER;
static char *admin_key = NULL;

typedef struct janus_videoroom_session {
	janus_plugin_session *handle;
	gint64 sdp_sessid;
	gint64 sdp_version;
	janus_videoroom_p_type participant_type;
	gpointer participant;
	gboolean started;
	gboolean stopping;
	volatile gint hangingup;
	volatile gint destroyed;
	janus_mutex mutex;
	janus_refcount ref;
} janus_videoroom_session;
static GHashTable *sessions;
static janus_mutex sessions_mutex = JANUS_MUTEX_INITIALIZER;

/* A host whose ports gets streamed RTP packets of the corresponding type */
typedef struct janus_videoroom_srtp_context janus_videoroom_srtp_context;
typedef struct janus_videoroom_rtp_forwarder {
	gboolean is_video;
	gboolean is_data;
	uint32_t ssrc;
	int payload_type;
	int substream;
	struct sockaddr_in serv_addr;
	/* Only needed for SRTP forwarders */
	gboolean is_srtp;
	janus_videoroom_srtp_context *srtp_ctx;
} janus_videoroom_rtp_forwarder;
/* SRTP encryption may be needed, and potentially shared */
struct janus_videoroom_srtp_context {
	GHashTable *contexts;
	char *id;
	srtp_t ctx;
	srtp_policy_t policy;
	char sbuf[1500];
	int slen;
	/* Keep track of how many forwarders are using this context */
	uint8_t count;
};

typedef struct janus_videoroom_publisher {
	janus_videoroom_session *session;
	janus_videoroom *room;	/* Room */
	guint64 room_id;	/* Unique room ID */
	guint64 user_id;	/* Unique ID in the room */
	guint32 pvt_id;		/* This is sent to the publisher for mapping purposes, but shouldn't be shared with others */
	gchar *display;		/* Display name (just for fun) */
	gchar *sdp;			/* The SDP this publisher negotiated, if any */
	gboolean audio, video, data;		/* Whether audio, video and/or data is going to be sent by this publisher */
	janus_videoroom_audiocodec acodec;	/* Audio codec this publisher is using */
	janus_videoroom_videocodec vcodec;	/* Video codec this publisher is using */
	guint32 audio_pt;		/* Audio payload type (Opus) */
	guint32 video_pt;		/* Video payload type (depends on room configuration) */
	guint32 audio_ssrc;		/* Audio SSRC of this publisher */
	guint32 video_ssrc;		/* Video SSRC of this publisher */
	uint32_t ssrc[3];		/* Only needed in case VP8 simulcasting is involved */
	int rtpmapid_extmap_id;	/* Only needed in case Firefox's RID-based simulcasting is involved */
	char *rid[3];			/* Only needed in case Firefox's RID-based simulcasting is involved */
	guint8 audio_level_extmap_id;		/* Audio level extmap ID */
	guint8 video_orient_extmap_id;		/* Video orientation extmap ID */
	guint8 playout_delay_extmap_id;		/* Playout delay extmap ID */
	guint8 transport_wide_cc_extmap_id; /* Transport wide cc extmap ID */
	gboolean audio_active;
	gboolean video_active;
	int audio_dBov_level;		/* Value in dBov of the audio level (last value from extension) */
	int audio_active_packets;	/* Participant's number of audio packets to accumulate */
	int audio_dBov_sum;			/* Participant's accumulated dBov value for audio level*/
	gboolean talking;			/* Whether this participant is currently talking (uses audio levels extension) */
	gboolean data_active;
	gboolean firefox;	/* We send Firefox users a different kind of FIR */
	uint32_t bitrate;
	gint64 remb_startup;/* Incremental changes on REMB to reach the target at startup */
	gint64 remb_latest;	/* Time of latest sent REMB (to avoid flooding) */
	gint64 fir_latest;	/* Time of latest sent FIR (to avoid flooding) */
	gint fir_seq;		/* FIR sequence number */
	gboolean recording_active;	/* Whether this publisher has to be recorded or not */
	gchar *recording_base;	/* Base name for the recording (e.g., /path/to/filename, will generate /path/to/filename-audio.mjr and/or /path/to/filename-video.mjr */
	janus_recorder *arc;	/* The Janus recorder instance for this publisher's audio, if enabled */
	janus_recorder *vrc;	/* The Janus recorder instance for this user's video, if enabled */
	janus_recorder *drc;	/* The Janus recorder instance for this publisher's data, if enabled */
	janus_mutex rec_mutex;	/* Mutex to protect the recorders from race conditions */
	GSList *subscribers;	/* Subscriptions to this publisher (who's watching this publisher)  */
	GSList *subscriptions;	/* Subscriptions this publisher has created (who this publisher is watching) */
	janus_mutex subscribers_mutex;
	GHashTable *rtp_forwarders;
	GHashTable *srtp_contexts;
	janus_mutex rtp_forwarders_mutex;
	int udp_sock; /* The udp socket on which to forward rtp packets */
	gboolean kicked;	/* Whether this participant has been kicked */
	volatile gint destroyed;
	janus_refcount ref;
} janus_videoroom_publisher;
static void janus_videoroom_rtp_forwarder_free_helper(gpointer data);
static void janus_videoroom_srtp_context_free_helper(gpointer data);
static guint32 janus_videoroom_rtp_forwarder_add_helper(janus_videoroom_publisher *p,
	const gchar* host, int port, int pt, uint32_t ssrc,
	int srtp_suite, const char *srtp_crypto,
	int substream, gboolean is_video, gboolean is_data);

typedef struct janus_videoroom_subscriber {
	janus_videoroom_session *session;
	janus_videoroom *room;	/* Room */
	guint64 room_id;		/* Unique room ID */
	janus_videoroom_publisher *feed;	/* Participant this subscriber is subscribed to */
	gboolean close_pc;		/* Whether we should automatically close the PeerConnection when the publisher goes away */
	guint32 pvt_id;			/* Private ID of the participant that is subscribing (if available/provided) */
	janus_sdp *sdp;			/* Offer we sent this listener (may be updated within renegotiations) */
	janus_rtp_switching_context context;	/* Needed in case there are publisher switches on this subscriber */
	int substream;			/* Which VP8 simulcast substream we should forward, in case the publisher is simulcasting */
	int substream_target;	/* As above, but to handle transitions (e.g., wait for keyframe) */
	int templayer;			/* Which VP8 simulcast temporal layer we should forward, in case the publisher is simulcasting */
	int templayer_target;	/* As above, but to handle transitions (e.g., wait for keyframe) */
	gint64 last_relayed;	/* When we relayed the last packet (used to detect when substreams become unavailable) */
	janus_vp8_simulcast_context simulcast_context;
	gboolean audio, video, data;		/* Whether audio, video and/or data must be sent to this subscriber */
	/* As above, but can't change dynamically (says whether something was negotiated at all in SDP) */
	gboolean audio_offered, video_offered, data_offered;
	gboolean paused;
	gboolean kicked;	/* Whether this subscription belongs to a participant that has been kicked */
	/* The following are only relevant if we're doing VP9 SVC, and are not to be confused with VP8
	 * simulcast, which has similar info (substream/templayer) but in a completely different context */
	int spatial_layer, target_spatial_layer;
	int temporal_layer, target_temporal_layer;
	volatile gint destroyed;
	janus_refcount ref;
} janus_videoroom_subscriber;

typedef struct janus_videoroom_rtp_relay_packet {
	janus_rtp_header *data;
	gint length;
	gboolean is_video;
	uint32_t ssrc[3];
	uint32_t timestamp;
	uint16_t seq_number;
	/* The following are only relevant if we're doing VP9 SVC*/
	gboolean svc;
	int spatial_layer;
	int temporal_layer;
	uint8_t pbit, dbit, ubit, bbit, ebit;
} janus_videoroom_rtp_relay_packet;


/* Freeing stuff */
static void janus_videoroom_subscriber_destroy(janus_videoroom_subscriber *s) {
	if(s && g_atomic_int_compare_and_exchange(&s->destroyed, 0, 1))
		janus_refcount_decrease(&s->ref);
}

static void janus_videoroom_subscriber_free(const janus_refcount *s_ref) {
	janus_videoroom_subscriber *s = janus_refcount_containerof(s_ref, janus_videoroom_subscriber, ref);
	/* This subscriber can be destroyed, free all the resources */
	janus_sdp_destroy(s->sdp);
	g_free(s);
}

static void janus_videoroom_publisher_dereference(janus_videoroom_publisher *p) {
	/* This is used by g_pointer_clear and g_hash_table_new_full so that NULL is only possible if that was inserted into the hash table. */
	janus_refcount_decrease(&p->ref);
}

static void janus_videoroom_publisher_dereference_nodebug(janus_videoroom_publisher *p) {
	janus_refcount_decrease_nodebug(&p->ref);
}

static void janus_videoroom_publisher_destroy(janus_videoroom_publisher *p) {
	if(p && g_atomic_int_compare_and_exchange(&p->destroyed, 0, 1))
		janus_refcount_decrease(&p->ref);
}

static void janus_videoroom_publisher_free(const janus_refcount *p_ref) {
	janus_videoroom_publisher *p = janus_refcount_containerof(p_ref, janus_videoroom_publisher, ref);
	g_free(p->display);
	p->display = NULL;
	g_free(p->sdp);
	p->sdp = NULL;
	g_free(p->recording_base);
	p->recording_base = NULL;
	janus_recorder_destroy(p->arc);
	janus_recorder_destroy(p->vrc);
	janus_recorder_destroy(p->drc);

	if(p->udp_sock > 0)
		close(p->udp_sock);
	g_hash_table_destroy(p->rtp_forwarders);
	p->rtp_forwarders = NULL;
	g_hash_table_destroy(p->srtp_contexts);
	p->srtp_contexts = NULL;
	g_slist_free(p->subscribers);

	janus_mutex_destroy(&p->subscribers_mutex);
	janus_mutex_destroy(&p->rtp_forwarders_mutex);
	g_free(p);
}

static void janus_videoroom_session_destroy(janus_videoroom_session *session) {
	if(session && g_atomic_int_compare_and_exchange(&session->destroyed, 0, 1))
		janus_refcount_decrease(&session->ref);
}

static void janus_videoroom_session_free(const janus_refcount *session_ref) {
	janus_videoroom_session *session = janus_refcount_containerof(session_ref, janus_videoroom_session, ref);
	/* Remove the reference to the core plugin session */
	janus_refcount_decrease(&session->handle->ref);
	/* This session can be destroyed, free all the resources */
	janus_mutex_destroy(&session->mutex);
	g_free(session);
}

static void janus_videoroom_room_dereference(janus_videoroom *room) {
	janus_refcount_decrease(&room->ref);
}

static void janus_videoroom_room_destroy(janus_videoroom *room) {
	if(room && g_atomic_int_compare_and_exchange(&room->destroyed, 0, 1))
		janus_refcount_decrease(&room->ref);
}

static void janus_videoroom_room_free(const janus_refcount *room_ref) {
	janus_videoroom *room = janus_refcount_containerof(room_ref, janus_videoroom, ref);
	/* This room can be destroyed, free all the resources */
	g_free(room->room_name);
	g_free(room->room_secret);
	g_free(room->room_pin);
	g_free(room->rec_dir);
	g_hash_table_destroy(room->participants);
	g_hash_table_destroy(room->private_ids);
	g_hash_table_destroy(room->allowed);
	g_free(room);
}

static void janus_videoroom_message_free(janus_videoroom_message *msg) {
	if(!msg || msg == &exit_message)
		return;

	if(msg->handle && msg->handle->plugin_handle) {
		janus_videoroom_session *session = (janus_videoroom_session *)msg->handle->plugin_handle;
		janus_refcount_decrease(&session->ref);
	}
	msg->handle = NULL;

	g_free(msg->transaction);
	msg->transaction = NULL;
	if(msg->message)
		json_decref(msg->message);
	msg->message = NULL;
	if(msg->jsep)
		json_decref(msg->jsep);
	msg->jsep = NULL;

	g_free(msg);
}


/* Error codes */
#define JANUS_VIDEOROOM_ERROR_UNKNOWN_ERROR		499
#define JANUS_VIDEOROOM_ERROR_NO_MESSAGE		421
#define JANUS_VIDEOROOM_ERROR_INVALID_JSON		422
#define JANUS_VIDEOROOM_ERROR_INVALID_REQUEST	423
#define JANUS_VIDEOROOM_ERROR_JOIN_FIRST		424
#define JANUS_VIDEOROOM_ERROR_ALREADY_JOINED	425
#define JANUS_VIDEOROOM_ERROR_NO_SUCH_ROOM		426
#define JANUS_VIDEOROOM_ERROR_ROOM_EXISTS		427
#define JANUS_VIDEOROOM_ERROR_NO_SUCH_FEED		428
#define JANUS_VIDEOROOM_ERROR_MISSING_ELEMENT	429
#define JANUS_VIDEOROOM_ERROR_INVALID_ELEMENT	430
#define JANUS_VIDEOROOM_ERROR_INVALID_SDP_TYPE	431
#define JANUS_VIDEOROOM_ERROR_PUBLISHERS_FULL	432
#define JANUS_VIDEOROOM_ERROR_UNAUTHORIZED		433
#define JANUS_VIDEOROOM_ERROR_ALREADY_PUBLISHED	434
#define JANUS_VIDEOROOM_ERROR_NOT_PUBLISHED		435
#define JANUS_VIDEOROOM_ERROR_ID_EXISTS			436
#define JANUS_VIDEOROOM_ERROR_INVALID_SDP		437


static guint32 janus_videoroom_rtp_forwarder_add_helper(janus_videoroom_publisher *p,
		const gchar *host, int port, int pt, uint32_t ssrc,
		int srtp_suite, const char *srtp_crypto,
		int substream, gboolean is_video, gboolean is_data) {
	if(!p || !host) {
		return 0;
	}
	janus_mutex_lock(&p->rtp_forwarders_mutex);
	janus_videoroom_rtp_forwarder *forward = g_malloc0(sizeof(janus_videoroom_rtp_forwarder));
	/* First of all, let's check if we need to setup an SRTP forwarder */
	if(!is_data && srtp_suite > 0 && srtp_crypto != NULL) {
		/* First of all, let's check if there's already an RTP forwarder with
		 * the same SRTP context: make sure SSRC and pt are the same too */
		char media[10];
		memset(media, 0, sizeof(media));
		if(!is_video) {
			g_sprintf(media, "audio");
		} else if(is_video) {
			g_sprintf(media, "video%d", substream);
		}
		char srtp_id[256];
		memset(srtp_id, 0, sizeof(srtp_id));
		g_snprintf(srtp_id, 255, "%s-%s-%"SCNu32"-%d", srtp_crypto, media, ssrc, pt);
		JANUS_LOG(LOG_VERB, "SRTP context ID: %s\n", srtp_id);
		janus_videoroom_srtp_context *srtp_ctx = g_hash_table_lookup(p->srtp_contexts, srtp_id);
		if(srtp_ctx != NULL) {
			JANUS_LOG(LOG_VERB, "  -- Reusing existing SRTP context\n");
			srtp_ctx->count++;
			forward->srtp_ctx = srtp_ctx;
		} else {
			/* Nope, base64 decode the crypto string and set it as a new SRTP context */
			JANUS_LOG(LOG_VERB, "  -- Creating new SRTP context\n");
			srtp_ctx = g_malloc0(sizeof(janus_videoroom_srtp_context));
			gsize len = 0;
			guchar *decoded = g_base64_decode(srtp_crypto, &len);
			if(len < SRTP_MASTER_LENGTH) {
				janus_mutex_unlock(&p->rtp_forwarders_mutex);
				JANUS_LOG(LOG_ERR, "Invalid SRTP crypto (%s)\n", srtp_crypto);
				g_free(decoded);
				g_free(srtp_ctx);
				g_free(forward);
				return 0;
			}
			/* Set SRTP policy */
			srtp_policy_t *policy = &srtp_ctx->policy;
			srtp_crypto_policy_set_rtp_default(&(policy->rtp));
			if(srtp_suite == 32) {
				srtp_crypto_policy_set_aes_cm_128_hmac_sha1_32(&(policy->rtp));
			} else if(srtp_suite == 80) {
				srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&(policy->rtp));
			}
			policy->ssrc.type = ssrc_any_inbound;
			policy->key = decoded;
			policy->next = NULL;
			/* Create SRTP context */
			srtp_err_status_t res = srtp_create(&srtp_ctx->ctx, policy);
			if(res != srtp_err_status_ok) {
				/* Something went wrong... */
				janus_mutex_unlock(&p->rtp_forwarders_mutex);
				JANUS_LOG(LOG_ERR, "Error creating forwarder SRTP session: %d (%s)\n", res, janus_srtp_error_str(res));
				g_free(decoded);
				policy->key = NULL;
				g_free(srtp_ctx);
				g_free(forward);
				return 0;
			}
			srtp_ctx->contexts = p->srtp_contexts;
			srtp_ctx->id = g_strdup(srtp_id);
			srtp_ctx->count = 1;
			g_hash_table_insert(p->srtp_contexts, srtp_ctx->id, srtp_ctx);
			forward->srtp_ctx = srtp_ctx;
		}
		forward->is_srtp = TRUE;
	}
	forward->is_video = is_video;
	forward->payload_type = pt;
	forward->ssrc = ssrc;
	forward->substream = substream;
	forward->is_data = is_data;
	forward->serv_addr.sin_family = AF_INET;
	inet_pton(AF_INET, host, &(forward->serv_addr.sin_addr));
	forward->serv_addr.sin_port = htons(port);
	guint32 stream_id = janus_random_uint32();
	while(g_hash_table_lookup(p->rtp_forwarders, GUINT_TO_POINTER(stream_id)) != NULL) {
		stream_id = janus_random_uint32();
	}
	g_hash_table_insert(p->rtp_forwarders, GUINT_TO_POINTER(stream_id), forward);
	janus_mutex_unlock(&p->rtp_forwarders_mutex);
	JANUS_LOG(LOG_VERB, "Added %s/%d rtp_forward to participant %"SCNu64" host: %s:%d stream_id: %"SCNu32"\n",
		is_data ? "data" : (is_video ? "video" : "audio"), substream, p->user_id, host, port, stream_id);
	return stream_id;
}

static void janus_videoroom_rtp_forwarder_free_helper(gpointer data) {
	if(data) {
		janus_videoroom_rtp_forwarder *forward = (janus_videoroom_rtp_forwarder *)data;
		if(forward) {
			if(forward->is_srtp && forward->srtp_ctx) {
				forward->srtp_ctx->count--;
				if(forward->srtp_ctx->count == 0 && forward->srtp_ctx->contexts != NULL)
					g_hash_table_remove(forward->srtp_ctx->contexts, forward->srtp_ctx->id);
			}
			g_free(forward);
			forward = NULL;
		}
	}
}

static void janus_videoroom_srtp_context_free_helper(gpointer data) {
	if(data) {
		janus_videoroom_srtp_context *srtp_ctx = (janus_videoroom_srtp_context *)data;
		if(srtp_ctx) {
			g_free(srtp_ctx->id);
			srtp_dealloc(srtp_ctx->ctx);
			g_free(srtp_ctx->policy.key);
			g_free(srtp_ctx);
			srtp_ctx = NULL;
		}
	}
}


/* Plugin implementation */
int janus_videoroom_init(janus_callbacks *callback, const char *config_path) {
	if(g_atomic_int_get(&stopping)) {
		/* Still stopping from before */
		return -1;
	}
	if(callback == NULL || config_path == NULL) {
		/* Invalid arguments */
		return -1;
	}

	/* Read configuration */
	char filename[255];
	g_snprintf(filename, 255, "%s/%s.cfg", config_path, JANUS_VIDEOROOM_PACKAGE);
	JANUS_LOG(LOG_VERB, "Configuration file: %s\n", filename);
	config = janus_config_parse(filename);
	config_folder = config_path;
	if(config != NULL)
		janus_config_print(config);

	rooms = g_hash_table_new_full(g_int64_hash, g_int64_equal,
		(GDestroyNotify)g_free, (GDestroyNotify) janus_videoroom_room_destroy);
	sessions = g_hash_table_new_full(NULL, NULL, NULL, (GDestroyNotify)janus_videoroom_session_destroy);

	messages = g_async_queue_new_full((GDestroyNotify) janus_videoroom_message_free);

	/* This is the callback we'll need to invoke to contact the gateway */
	gateway = callback;

	/* Parse configuration to populate the rooms list */
	if(config != NULL) {
		/* Any admin key to limit who can "create"? */
		janus_config_item *key = janus_config_get_item_drilldown(config, "general", "admin_key");
		if(key != NULL && key->value != NULL)
			admin_key = g_strdup(key->value);
		janus_config_item *events = janus_config_get_item_drilldown(config, "general", "events");
		if(events != NULL && events->value != NULL)
			notify_events = janus_is_true(events->value);
		if(!notify_events && callback->events_is_enabled()) {
			JANUS_LOG(LOG_WARN, "Notification of events to handlers disabled for %s\n", JANUS_VIDEOROOM_NAME);
		}
		/* Iterate on all rooms */
		GList *cl = janus_config_get_categories(config);
		while(cl != NULL) {
			janus_config_category *cat = (janus_config_category *)cl->data;
			if(cat->name == NULL || !strcasecmp(cat->name, "general")) {
				cl = cl->next;
				continue;
			}
			JANUS_LOG(LOG_VERB, "Adding video room '%s'\n", cat->name);
			janus_config_item *desc = janus_config_get_item(cat, "description");
			janus_config_item *priv = janus_config_get_item(cat, "is_private");
			janus_config_item *secret = janus_config_get_item(cat, "secret");
			janus_config_item *pin = janus_config_get_item(cat, "pin");
			janus_config_item *req_pvtid = janus_config_get_item(cat, "require_pvtid");
			janus_config_item *bitrate = janus_config_get_item(cat, "bitrate");
			janus_config_item *maxp = janus_config_get_item(cat, "publishers");
			janus_config_item *firfreq = janus_config_get_item(cat, "fir_freq");
			janus_config_item *audiocodec = janus_config_get_item(cat, "audiocodec");
			janus_config_item *videocodec = janus_config_get_item(cat, "videocodec");
			janus_config_item *svc = janus_config_get_item(cat, "video_svc");
			janus_config_item *audiolevel_ext = janus_config_get_item(cat, "audiolevel_ext");
			janus_config_item *audiolevel_event = janus_config_get_item(cat, "audiolevel_event");
			janus_config_item *audio_active_packets = janus_config_get_item(cat, "audio_active_packets");
			janus_config_item *audio_level_average = janus_config_get_item(cat, "audio_level_average");
			janus_config_item *videoorient_ext = janus_config_get_item(cat, "videoorient_ext");
			janus_config_item *playoutdelay_ext = janus_config_get_item(cat, "playoutdelay_ext");
			janus_config_item *transport_wide_cc_ext = janus_config_get_item(cat, "transport_wide_cc_ext");
			janus_config_item *notify_joining = janus_config_get_item(cat, "notify_joining");
			janus_config_item *record = janus_config_get_item(cat, "record");
			janus_config_item *rec_dir = janus_config_get_item(cat, "rec_dir");
			/* Create the video room */
			janus_videoroom *videoroom = g_malloc0(sizeof(janus_videoroom));
			videoroom->room_id = g_ascii_strtoull(cat->name, NULL, 0);
			char *description = NULL;
			if(desc != NULL && desc->value != NULL && strlen(desc->value) > 0)
				description = g_strdup(desc->value);
			else
				description = g_strdup(cat->name);
			videoroom->room_name = description;
			if(secret != NULL && secret->value != NULL) {
				videoroom->room_secret = g_strdup(secret->value);
			}
			if(pin != NULL && pin->value != NULL) {
				videoroom->room_pin = g_strdup(pin->value);
			}
			videoroom->is_private = priv && priv->value && janus_is_true(priv->value);
			videoroom->require_pvtid = req_pvtid && req_pvtid->value && janus_is_true(req_pvtid->value);
			videoroom->max_publishers = 3;	/* FIXME How should we choose a default? */
			if(maxp != NULL && maxp->value != NULL)
				videoroom->max_publishers = atol(maxp->value);
			if(videoroom->max_publishers < 0)
				videoroom->max_publishers = 3;	/* FIXME How should we choose a default? */
			videoroom->bitrate = 0;
			if(bitrate != NULL && bitrate->value != NULL)
				videoroom->bitrate = atol(bitrate->value);
			if(videoroom->bitrate > 0 && videoroom->bitrate < 64000)
				videoroom->bitrate = 64000;	/* Don't go below 64k */
			videoroom->fir_freq = 0;
			if(firfreq != NULL && firfreq->value != NULL)
				videoroom->fir_freq = atol(firfreq->value);
			/* By default, we force Opus as the only audio codec */
			videoroom->acodec[0] = JANUS_VIDEOROOM_OPUS;
			videoroom->acodec[1] = JANUS_VIDEOROOM_NOAUDIO;
			videoroom->acodec[2] = JANUS_VIDEOROOM_NOAUDIO;
			/* Check if we're forcing a different single codec, or allowing more than one */
			if(audiocodec && audiocodec->value) {
				gchar **list = g_strsplit(audiocodec->value, ",", 4);
				gchar *codec = list[0];
				if(codec != NULL) {
					int i=0;
					while(codec != NULL) {
						if(i == 3) {
							JANUS_LOG(LOG_WARN, "Ignoring extra audio codecs: %s\n", codec);
							break;
						}
						if(strlen(codec) > 0)
							videoroom->acodec[i] = janus_videoroom_audiocodec_from_name(codec);
						i++;
						codec = list[i];
					}
				}
				g_clear_pointer(&list, g_strfreev);
			}
			/* By default, we force VP8 as the only video codec */
			videoroom->vcodec[0] = JANUS_VIDEOROOM_VP8;
			videoroom->vcodec[1] = JANUS_VIDEOROOM_NOVIDEO;
			videoroom->vcodec[2] = JANUS_VIDEOROOM_NOVIDEO;
			/* Check if we're forcing a different single codec, or allowing more than one */
			if(videocodec && videocodec->value) {
				gchar **list = g_strsplit(videocodec->value, ",", 4);
				gchar *codec = list[0];
				if(codec != NULL) {
					int i=0;
					while(codec != NULL) {
						if(i == 3) {
							JANUS_LOG(LOG_WARN, "Ignoring extra video codecs: %s\n", codec);
							break;
						}
						if(strlen(codec) > 0)
							videoroom->vcodec[i] = janus_videoroom_videocodec_from_name(codec);
						i++;
						codec = list[i];
					}
				}
				g_clear_pointer(&list, g_strfreev);
			}
			if(svc && svc->value && janus_is_true(svc->value)) {
				if(videoroom->vcodec[0] == JANUS_VIDEOROOM_VP9 &&
						videoroom->vcodec[1] == JANUS_VIDEOROOM_NOVIDEO &&
						videoroom->vcodec[2] == JANUS_VIDEOROOM_NOVIDEO) {
					videoroom->do_svc = TRUE;
				} else {
					JANUS_LOG(LOG_WARN, "SVC is only supported, in an experimental way, for VP9 only rooms: disabling it...\n");
				}
			}
			videoroom->audiolevel_ext = TRUE;
			if(audiolevel_ext != NULL && audiolevel_ext->value != NULL)
				videoroom->audiolevel_ext = janus_is_true(audiolevel_ext->value);
			videoroom->audiolevel_event = FALSE;
			if(audiolevel_event != NULL && audiolevel_event->value != NULL)
				videoroom->audiolevel_event = janus_is_true(audiolevel_event->value);
			if(videoroom->audiolevel_event) {
				videoroom->audio_active_packets = 100;
				if(audio_active_packets != NULL && audio_active_packets->value != NULL){
					if(atoi(audio_active_packets->value) > 0) {
						videoroom->audio_active_packets = atoi(audio_active_packets->value);
					} else {
						JANUS_LOG(LOG_WARN, "Invalid audio_active_packets value, using default: %d\n", videoroom->audio_active_packets);
					}
				}
				videoroom->audio_level_average = 25;
				if(audio_level_average != NULL && audio_level_average->value != NULL) {
					if(atoi(audio_level_average->value) > 0) {
						videoroom->audio_level_average = atoi(audio_level_average->value);
					} else {
						JANUS_LOG(LOG_WARN, "Invalid audio_level_average value provided, using default: %d\n", videoroom->audio_level_average);
					}
				}
			}
			videoroom->videoorient_ext = TRUE;
			if(videoorient_ext != NULL && videoorient_ext->value != NULL)
				videoroom->videoorient_ext = janus_is_true(videoorient_ext->value);
			videoroom->playoutdelay_ext = TRUE;
			if(playoutdelay_ext != NULL && playoutdelay_ext->value != NULL)
				videoroom->playoutdelay_ext = janus_is_true(playoutdelay_ext->value);
			videoroom->transport_wide_cc_ext = FALSE;
			if(transport_wide_cc_ext != NULL && transport_wide_cc_ext->value != NULL)
				videoroom->transport_wide_cc_ext = janus_is_true(transport_wide_cc_ext->value);
			if(record && record->value) {
				videoroom->record = janus_is_true(record->value);
			}
			if(rec_dir && rec_dir->value) {
				videoroom->rec_dir = g_strdup(rec_dir->value);
			}
			/* By default, the videoroom plugin does not notify about participants simply joining the room.
			   It only notifies when the participant actually starts publishing media. */
			videoroom->notify_joining = FALSE;
			if(notify_joining != NULL && notify_joining->value != NULL)
				videoroom->notify_joining = janus_is_true(notify_joining->value);
			g_atomic_int_set(&videoroom->destroyed, 0);
			janus_mutex_init(&videoroom->mutex);
			janus_refcount_init(&videoroom->ref, janus_videoroom_room_free);
			videoroom->participants = g_hash_table_new_full(g_int64_hash, g_int64_equal, (GDestroyNotify)g_free, (GDestroyNotify)janus_videoroom_publisher_dereference);
			videoroom->private_ids = g_hash_table_new(NULL, NULL);
			videoroom->check_allowed = FALSE;	/* Static rooms can't have an "allowed" list yet, no hooks to the configuration file */
			videoroom->allowed = g_hash_table_new_full(g_str_hash, g_str_equal, (GDestroyNotify)g_free, NULL);
			janus_mutex_lock(&rooms_mutex);
			g_hash_table_insert(rooms, janus_uint64_dup(videoroom->room_id), videoroom);
			janus_mutex_unlock(&rooms_mutex);
			/* Compute a list of the supported codecs for the summary */
			char audio_codecs[100], video_codecs[100];
			memset(audio_codecs, 0, sizeof(audio_codecs));
			g_snprintf(audio_codecs, sizeof(audio_codecs), "%s", janus_videoroom_audiocodec_name(videoroom->acodec[0]));
			if(videoroom->acodec[1] != JANUS_VIDEOROOM_NOAUDIO) {
				g_strlcat(audio_codecs, "|", sizeof(audio_codecs));
				g_strlcat(audio_codecs, janus_videoroom_audiocodec_name(videoroom->acodec[1]), sizeof(audio_codecs));
			}
			if(videoroom->acodec[2] != JANUS_VIDEOROOM_NOAUDIO) {
				g_strlcat(audio_codecs, "|", sizeof(audio_codecs));
				g_strlcat(audio_codecs, janus_videoroom_audiocodec_name(videoroom->acodec[2]), sizeof(audio_codecs));
			}
			memset(video_codecs, 0, sizeof(video_codecs));
			g_snprintf(video_codecs, sizeof(video_codecs), "%s", janus_videoroom_videocodec_name(videoroom->vcodec[0]));
			if(videoroom->vcodec[1] != JANUS_VIDEOROOM_NOVIDEO) {
				g_strlcat(video_codecs, "|", sizeof(video_codecs));
				g_strlcat(video_codecs, janus_videoroom_videocodec_name(videoroom->vcodec[1]), sizeof(video_codecs));
			}
			if(videoroom->vcodec[2] != JANUS_VIDEOROOM_NOVIDEO) {
				g_strlcat(video_codecs, "|", sizeof(video_codecs));
				g_strlcat(video_codecs, janus_videoroom_videocodec_name(videoroom->vcodec[2]), sizeof(video_codecs));
			}
			JANUS_LOG(LOG_VERB, "Created videoroom: %"SCNu64" (%s, %s, %s/%s codecs, secret: %s, pin: %s, pvtid: %s)\n",
				videoroom->room_id, videoroom->room_name,
				videoroom->is_private ? "private" : "public",
				audio_codecs, video_codecs,
				videoroom->room_secret ? videoroom->room_secret : "no secret",
				videoroom->room_pin ? videoroom->room_pin : "no pin",
				videoroom->require_pvtid ? "required" : "optional");
			if(videoroom->record) {
				JANUS_LOG(LOG_VERB, "  -- Room is going to be recorded in %s\n", videoroom->rec_dir ? videoroom->rec_dir : "the current folder");
			}
			cl = cl->next;
		}
		/* Done: we keep the configuration file open in case we get a "create" or "destroy" with permanent=true */
	}

	/* Show available rooms */
	janus_mutex_lock(&rooms_mutex);
	GHashTableIter iter;
	gpointer value;
	g_hash_table_iter_init(&iter, rooms);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		janus_videoroom *vr = value;
		/* Compute a list of the supported codecs for the summary */
		char audio_codecs[100], video_codecs[100];
		memset(audio_codecs, 0, sizeof(audio_codecs));
		g_snprintf(audio_codecs, sizeof(audio_codecs), "%s", janus_videoroom_audiocodec_name(vr->acodec[0]));
		if(vr->acodec[1] != JANUS_VIDEOROOM_NOAUDIO) {
			g_strlcat(audio_codecs, "|", sizeof(audio_codecs));
			g_strlcat(audio_codecs, janus_videoroom_audiocodec_name(vr->acodec[1]), sizeof(audio_codecs));
		}
		if(vr->acodec[2] != JANUS_VIDEOROOM_NOAUDIO) {
			g_strlcat(audio_codecs, "|", sizeof(audio_codecs));
			g_strlcat(audio_codecs, janus_videoroom_audiocodec_name(vr->acodec[2]), sizeof(audio_codecs));
		}
		memset(video_codecs, 0, sizeof(video_codecs));
		g_snprintf(video_codecs, sizeof(video_codecs), "%s", janus_videoroom_videocodec_name(vr->vcodec[0]));
		if(vr->vcodec[1] != JANUS_VIDEOROOM_NOVIDEO) {
			g_strlcat(video_codecs, "|", sizeof(video_codecs));
			g_strlcat(video_codecs, janus_videoroom_videocodec_name(vr->vcodec[1]), sizeof(video_codecs));
		}
		if(vr->vcodec[2] != JANUS_VIDEOROOM_NOVIDEO) {
			g_strlcat(video_codecs, "|", sizeof(video_codecs));
			g_strlcat(video_codecs, janus_videoroom_videocodec_name(vr->vcodec[2]), sizeof(video_codecs));
		}
		JANUS_LOG(LOG_VERB, "  ::: [%"SCNu64"][%s] %"SCNu32", max %d publishers, FIR frequency of %d seconds, %s audio codec(s), %s video codec(s)\n",
			vr->room_id, vr->room_name, vr->bitrate, vr->max_publishers, vr->fir_freq,
			audio_codecs, video_codecs);
	}
	janus_mutex_unlock(&rooms_mutex);

	g_atomic_int_set(&initialized, 1);

	/* Launch the thread that will handle incoming messages */
	GError *error = NULL;
	handler_thread = g_thread_try_new("videoroom handler", janus_videoroom_handler, NULL, &error);
	if(error != NULL) {
		g_atomic_int_set(&initialized, 0);
		JANUS_LOG(LOG_ERR, "Got error %d (%s) trying to launch the VideoRoom handler thread...\n", error->code, error->message ? error->message : "??");
		janus_config_destroy(config);
		return -1;
	}
	JANUS_LOG(LOG_INFO, "%s initialized!\n", JANUS_VIDEOROOM_NAME);
	return 0;
}

void janus_videoroom_destroy(void) {
	if(!g_atomic_int_get(&initialized))
		return;
	g_atomic_int_set(&stopping, 1);

	g_async_queue_push(messages, &exit_message);
	if(handler_thread != NULL) {
		g_thread_join(handler_thread);
		handler_thread = NULL;
	}

	/* FIXME We should destroy the sessions cleanly */
	janus_mutex_lock(&sessions_mutex);
	g_hash_table_destroy(sessions);
	janus_mutex_unlock(&sessions_mutex);

	janus_mutex_lock(&rooms_mutex);
	g_hash_table_destroy(rooms);
	janus_mutex_unlock(&rooms_mutex);

	g_async_queue_unref(messages);
	messages = NULL;

	janus_config_destroy(config);
	g_free(admin_key);

	g_atomic_int_set(&initialized, 0);
	g_atomic_int_set(&stopping, 0);
	JANUS_LOG(LOG_INFO, "%s destroyed!\n", JANUS_VIDEOROOM_NAME);
}

int janus_videoroom_get_api_compatibility(void) {
	/* Important! This is what your plugin MUST always return: don't lie here or bad things will happen */
	return JANUS_PLUGIN_API_VERSION;
}

int janus_videoroom_get_version(void) {
	return JANUS_VIDEOROOM_VERSION;
}

const char *janus_videoroom_get_version_string(void) {
	return JANUS_VIDEOROOM_VERSION_STRING;
}

const char *janus_videoroom_get_description(void) {
	return JANUS_VIDEOROOM_DESCRIPTION;
}

const char *janus_videoroom_get_name(void) {
	return JANUS_VIDEOROOM_NAME;
}

const char *janus_videoroom_get_author(void) {
	return JANUS_VIDEOROOM_AUTHOR;
}

const char *janus_videoroom_get_package(void) {
	return JANUS_VIDEOROOM_PACKAGE;
}

static janus_videoroom_session *janus_videoroom_lookup_session(janus_plugin_session *handle) {
	janus_videoroom_session *session = NULL;
	if (g_hash_table_contains(sessions, handle)) {
		session = (janus_videoroom_session *)handle->plugin_handle;
	}
	return session;
}

void janus_videoroom_create_session(janus_plugin_session *handle, int *error) {
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized)) {
		*error = -1;
		return;
	}	
	janus_videoroom_session *session = g_malloc0(sizeof(janus_videoroom_session));
	session->handle = handle;
	session->participant_type = janus_videoroom_p_type_none;
	session->participant = NULL;
	g_atomic_int_set(&session->hangingup, 0);
	g_atomic_int_set(&session->destroyed, 0);
	handle->plugin_handle = session;
	janus_mutex_init(&session->mutex);
	janus_refcount_init(&session->ref, janus_videoroom_session_free);

	janus_mutex_lock(&sessions_mutex);
	g_hash_table_insert(sessions, handle, session);
	janus_mutex_unlock(&sessions_mutex);

	return;
}

static janus_videoroom_publisher *janus_videoroom_session_get_publisher(janus_videoroom_session *session) {
	janus_mutex_lock(&session->mutex);
	janus_videoroom_publisher *publisher = (janus_videoroom_publisher *)session->participant;
	if(publisher)
		janus_refcount_increase(&publisher->ref);
	janus_mutex_unlock(&session->mutex);
	return publisher;
}

static janus_videoroom_publisher *janus_videoroom_session_get_publisher_nodebug(janus_videoroom_session *session) {
	janus_mutex_lock(&session->mutex);
	janus_videoroom_publisher *publisher = (janus_videoroom_publisher *)session->participant;
	if(publisher)
		janus_refcount_increase_nodebug(&publisher->ref);
	janus_mutex_unlock(&session->mutex);
	return publisher;
}

static void janus_videoroom_notify_participants(janus_videoroom_publisher *participant, json_t *msg) {
	/* participant->room->mutex has to be locked. */
	if(participant->room == NULL)
		return;
	GHashTableIter iter;
	gpointer value;
	g_hash_table_iter_init(&iter, participant->room->participants);
	while (participant->room && !g_atomic_int_get(&participant->room->destroyed) && g_hash_table_iter_next(&iter, NULL, &value)) {
		janus_videoroom_publisher *p = value;
		if(p && p->session && p != participant) {
			JANUS_LOG(LOG_VERB, "Notifying participant %"SCNu64" (%s)\n", p->user_id, p->display ? p->display : "??");
			int ret = gateway->push_event(p->session->handle, &janus_videoroom_plugin, NULL, msg, NULL);
			JANUS_LOG(LOG_VERB, "  >> %d (%s)\n", ret, janus_get_api_error(ret));
		}
	}
}

static void janus_videoroom_participant_joining(janus_videoroom_publisher *p) {
	/* we need to check if the room still exists, may have been destroyed already */
	if(p->room == NULL)
		return;
	janus_mutex_lock(&rooms_mutex);
	if (!g_hash_table_lookup(rooms, &p->room_id)) {
		JANUS_LOG(LOG_ERR, "No such room (%"SCNu64")\n", p->room_id);
		janus_mutex_unlock(&rooms_mutex);
		return;
	}
	janus_mutex_unlock(&rooms_mutex);
	if(!g_atomic_int_get(&p->room->destroyed) && p->room->notify_joining) {
		json_t *event = json_object();
		json_t *user = json_object();
		json_object_set_new(user, "id", json_integer(p->user_id));
		if (p->display) {
			json_object_set_new(user, "display", json_string(p->display));
		}
		json_object_set_new(event, "videoroom", json_string("event"));
		json_object_set_new(event, "room", json_integer(p->room_id));
		json_object_set_new(event, "joining", user);
		janus_videoroom_notify_participants(p, event);
		/* user gets deref-ed by the owner event */
		json_decref(event);
	}
}

static void janus_videoroom_leave_or_unpublish(janus_videoroom_publisher *participant, gboolean is_leaving, gboolean kicked) {
	/* we need to check if the room still exists, may have been destroyed already */
	if(participant->room == NULL)
		return;
	janus_mutex_lock(&rooms_mutex);
	if (!g_hash_table_lookup(rooms, &participant->room_id)) {
		JANUS_LOG(LOG_ERR, "No such room (%"SCNu64")\n", participant->room_id);
		janus_mutex_unlock(&rooms_mutex);
		return;
	}
	janus_mutex_unlock(&rooms_mutex);
	if(!participant->room || g_atomic_int_get(&participant->room->destroyed))
		return;
	janus_mutex_lock(&participant->room->mutex);
	json_t *event = json_object();
	json_object_set_new(event, "videoroom", json_string("event"));
	json_object_set_new(event, "room", json_integer(participant->room_id));
	json_object_set_new(event, is_leaving ? (kicked ? "kicked" : "leaving") : "unpublished",
		json_integer(participant->user_id));
	janus_videoroom_notify_participants(participant, event);
	if(is_leaving) {
		g_hash_table_remove(participant->room->participants, &participant->user_id);
		g_hash_table_remove(participant->room->private_ids, GUINT_TO_POINTER(participant->pvt_id));
	}
	json_decref(event);
	janus_mutex_unlock(&participant->room->mutex);
}

void janus_videoroom_destroy_session(janus_plugin_session *handle, int *error) {
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized)) {
		*error = -1;
		return;
	}
	janus_mutex_lock(&sessions_mutex);
	janus_videoroom_session *session = janus_videoroom_lookup_session(handle);
	if(!session) {
		janus_mutex_unlock(&sessions_mutex);
		JANUS_LOG(LOG_ERR, "No VideoRoom session associated with this handle...\n");
		*error = -2;
		return;
	}
	if(g_atomic_int_get(&session->destroyed)) {
		janus_mutex_unlock(&sessions_mutex);
		JANUS_LOG(LOG_WARN, "VideoRoom session already marked as destroyed...\n");
		return;
	}
	/* Cleaning up and removing the session is done in a lazy way */
	if(!g_atomic_int_get(&session->destroyed)) {
		/* Any related WebRTC PeerConnection is not available anymore either */
		janus_videoroom_hangup_media_internal(handle);
		if(session->participant_type == janus_videoroom_p_type_publisher) {
			/* Get rid of publisher */
			janus_mutex_lock(&session->mutex);
			janus_videoroom_publisher *p = (janus_videoroom_publisher *)session->participant;
			if(p)
				janus_refcount_increase(&p->ref);
			session->participant = NULL;
			janus_mutex_unlock(&session->mutex);
			if(p && p->room) {
				janus_videoroom_leave_or_unpublish(p, TRUE, FALSE);
				/* Don't clear p->room.  Another thread calls janus_videoroom_leave_or_unpublish,
					 too, and there is no mutex to protect this change. */
				g_clear_pointer(&p->room, janus_videoroom_room_dereference);
			}
			janus_videoroom_publisher_destroy(p);
			if(p)
				janus_refcount_decrease(&p->ref);
		} else if(session->participant_type == janus_videoroom_p_type_subscriber) {
			janus_videoroom_subscriber *s = (janus_videoroom_subscriber *)session->participant;
			session->participant = NULL;
			if(s->room) {
				janus_refcount_decrease(&s->room->ref);
			}
			janus_videoroom_subscriber_destroy(s);
		}
		g_hash_table_remove(sessions, handle);
	}
	janus_mutex_unlock(&sessions_mutex);
	return;
}

json_t *janus_videoroom_query_session(janus_plugin_session *handle) {
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized)) {
		return NULL;
	}	
	janus_mutex_lock(&sessions_mutex);
	janus_videoroom_session *session = janus_videoroom_lookup_session(handle);
	if(!session) {
		janus_mutex_unlock(&sessions_mutex);
		JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
		return NULL;
	}
	janus_refcount_increase(&session->ref);
	janus_mutex_unlock(&sessions_mutex);
	/* Show the participant/room info, if any */
	json_t *info = json_object();
	if(session->participant) {
		if(session->participant_type == janus_videoroom_p_type_none) {
			json_object_set_new(info, "type", json_string("none"));
		} else if(session->participant_type == janus_videoroom_p_type_publisher) {
			json_object_set_new(info, "type", json_string("publisher"));
			janus_videoroom_publisher *participant = janus_videoroom_session_get_publisher(session);
			if(participant && participant->room) {
				janus_videoroom *room = participant->room; 
				json_object_set_new(info, "room", room ? json_integer(room->room_id) : NULL);
				json_object_set_new(info, "id", json_integer(participant->user_id));
				json_object_set_new(info, "private_id", json_integer(participant->pvt_id));
				if(participant->display)
					json_object_set_new(info, "display", json_string(participant->display));
				if(participant->subscribers)
					json_object_set_new(info, "viewers", json_integer(g_slist_length(participant->subscribers)));
				json_t *media = json_object();
				json_object_set_new(media, "audio", participant->audio ? json_true() : json_false());
				if(participant->audio)
					json_object_set_new(media, "audio_codec", json_string(janus_videoroom_audiocodec_name(participant->acodec)));
				json_object_set_new(media, "video", participant->video ? json_true() : json_false());
				if(participant->video)
					json_object_set_new(media, "video_codec", json_string(janus_videoroom_videocodec_name(participant->vcodec)));
				json_object_set_new(media, "data", participant->data ? json_true() : json_false());
				json_object_set_new(info, "media", media);
				json_object_set_new(info, "bitrate", json_integer(participant->bitrate));
				if(participant->ssrc[0] != 0)
					json_object_set_new(info, "simulcast", json_true());
				if(participant->arc || participant->vrc || participant->drc) {
					json_t *recording = json_object();
					if(participant->arc && participant->arc->filename)
						json_object_set_new(recording, "audio", json_string(participant->arc->filename));
					if(participant->vrc && participant->vrc->filename)
						json_object_set_new(recording, "video", json_string(participant->vrc->filename));
					if(participant->drc && participant->drc->filename)
						json_object_set_new(recording, "data", json_string(participant->drc->filename));
					json_object_set_new(info, "recording", recording);
				}
				if(participant->audio_level_extmap_id > 0) {
					json_object_set_new(info, "audio-level-dBov", json_integer(participant->audio_dBov_level));
					json_object_set_new(info, "talking", participant->talking ? json_true() : json_false());
				}
				janus_refcount_decrease(&participant->ref);
			}
		} else if(session->participant_type == janus_videoroom_p_type_subscriber) {
			json_object_set_new(info, "type", json_string("subscriber"));
			janus_videoroom_subscriber *participant = (janus_videoroom_subscriber *)session->participant;
			if(participant && participant->room) {
				janus_videoroom_publisher *feed = (janus_videoroom_publisher *)participant->feed;
				if(feed && feed->room) {
					janus_videoroom *room = feed->room; 
					json_object_set_new(info, "room", room ? json_integer(room->room_id) : NULL);
					json_object_set_new(info, "private_id", json_integer(participant->pvt_id));
					json_object_set_new(info, "feed_id", json_integer(feed->user_id));
					if(feed->display)
						json_object_set_new(info, "feed_display", json_string(feed->display));
				}
				json_t *media = json_object();
				json_object_set_new(media, "audio", participant->audio ? json_true() : json_false());
				json_object_set_new(media, "audio-offered", participant->audio_offered ? json_true() : json_false());
				json_object_set_new(media, "video", participant->video ? json_true() : json_false());
				json_object_set_new(media, "video-offered", participant->video_offered ? json_true() : json_false());
				json_object_set_new(media, "data", participant->data ? json_true() : json_false());
				json_object_set_new(media, "data-offered", participant->data_offered ? json_true() : json_false());
				if(feed && feed->ssrc[0] != 0) {
					json_object_set_new(info, "simulcast", json_true());
					json_object_set_new(info, "substream", json_integer(participant->substream));
					json_object_set_new(info, "substream-target", json_integer(participant->substream_target));
					json_object_set_new(info, "temporal-layer", json_integer(participant->templayer));
					json_object_set_new(info, "temporal-layer-target", json_integer(participant->templayer_target));
				}
				json_object_set_new(info, "media", media);
				if(participant->room && participant->room->do_svc) {
					json_t *svc = json_object();
					json_object_set_new(svc, "spatial-layer", json_integer(participant->spatial_layer));
					json_object_set_new(svc, "target-spatial-layer", json_integer(participant->target_spatial_layer));
					json_object_set_new(svc, "temporal-layer", json_integer(participant->temporal_layer));
					json_object_set_new(svc, "target-temporal-layer", json_integer(participant->target_temporal_layer));
					json_object_set_new(info, "svc", svc);
				}
			}
		}
	}
	json_object_set_new(info, "hangingup", json_integer(g_atomic_int_get(&session->hangingup)));
	json_object_set_new(info, "destroyed", json_integer(g_atomic_int_get(&session->destroyed)));
	janus_refcount_decrease(&session->ref);
	return info;
}

static int janus_videoroom_access_room(json_t *root, gboolean check_modify, gboolean check_join, janus_videoroom **videoroom, char *error_cause, int error_cause_size) {
	/* rooms_mutex has to be locked */
	int error_code = 0;
	json_t *room = json_object_get(root, "room");
	guint64 room_id = json_integer_value(room);
	*videoroom = g_hash_table_lookup(rooms, &room_id);
	if(*videoroom == NULL) {
		JANUS_LOG(LOG_ERR, "No such room (%"SCNu64")\n", room_id);
		error_code = JANUS_VIDEOROOM_ERROR_NO_SUCH_ROOM;
		if(error_cause)
			g_snprintf(error_cause, error_cause_size, "No such room (%"SCNu64")", room_id);
		return error_code;
	}
	if((*videoroom)->destroyed) {
		JANUS_LOG(LOG_ERR, "No such room (%"SCNu64")\n", room_id);
		error_code = JANUS_VIDEOROOM_ERROR_NO_SUCH_ROOM;
		if(error_cause)
			g_snprintf(error_cause, error_cause_size, "No such room (%"SCNu64")", room_id);
		return error_code;
	}
	if(check_modify) {
		char error_cause2[100];
		JANUS_CHECK_SECRET((*videoroom)->room_secret, root, "secret", error_code, error_cause2,
			JANUS_VIDEOROOM_ERROR_MISSING_ELEMENT, JANUS_VIDEOROOM_ERROR_INVALID_ELEMENT, JANUS_VIDEOROOM_ERROR_UNAUTHORIZED);
		if(error_code != 0) {
			g_strlcpy(error_cause, error_cause2, error_cause_size);
			return error_code;
		}
	}
	if(check_join) {
		char error_cause2[100];
		/* signed tokens bypass pin validation */
		json_t *token = json_object_get(root, "token");
		if(token) {
			char room_descriptor[26];
			g_snprintf(room_descriptor, sizeof(room_descriptor), "room=%"SCNu64, room_id);
			if(gateway->auth_signature_contains(&janus_videoroom_plugin, json_string_value(token), room_descriptor))
				return 0;
		}
		JANUS_CHECK_SECRET((*videoroom)->room_pin, root, "pin", error_code, error_cause2,
			JANUS_VIDEOROOM_ERROR_MISSING_ELEMENT, JANUS_VIDEOROOM_ERROR_INVALID_ELEMENT, JANUS_VIDEOROOM_ERROR_UNAUTHORIZED);
		if(error_code != 0) {
			g_strlcpy(error_cause, error_cause2, error_cause_size);
			return error_code;
		}
	}
	return 0;
}

struct janus_plugin_result *janus_videoroom_handle_message(janus_plugin_session *handle, char *transaction, json_t *message, json_t *jsep) {
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return janus_plugin_result_new(JANUS_PLUGIN_ERROR, g_atomic_int_get(&stopping) ? "Shutting down" : "Plugin not initialized", NULL);

	/* Pre-parse the message */
	int error_code = 0;
	char error_cause[512];
	json_t *root = message;
	json_t *response = NULL;

	janus_mutex_lock(&sessions_mutex);
	janus_videoroom_session *session = janus_videoroom_lookup_session(handle);
	if(!session) {
		JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
		error_code = JANUS_VIDEOROOM_ERROR_UNKNOWN_ERROR;
		g_snprintf(error_cause, 512, "%s", "session associated with this handle...");
		goto plugin_response;
	}
	/* Increase the reference counter for this session: we'll decrease it after we handle the message */
	janus_refcount_increase(&session->ref);
	janus_mutex_unlock(&sessions_mutex);
	if(g_atomic_int_get(&session->destroyed)) {
		JANUS_LOG(LOG_ERR, "Session has already been marked as destroyed...\n");
		error_code = JANUS_VIDEOROOM_ERROR_UNKNOWN_ERROR;
		g_snprintf(error_cause, 512, "%s", "Session has already been marked as destroyed...");
		goto plugin_response;
	}

	if(message == NULL) {
		JANUS_LOG(LOG_ERR, "No message??\n");
		error_code = JANUS_VIDEOROOM_ERROR_NO_MESSAGE;
		g_snprintf(error_cause, 512, "%s", "No message??");
		goto plugin_response;
	}
	if(!json_is_object(root)) {
		JANUS_LOG(LOG_ERR, "JSON error: not an object\n");
		error_code = JANUS_VIDEOROOM_ERROR_INVALID_JSON;
		g_snprintf(error_cause, 512, "JSON error: not an object");
		goto plugin_response;
	}
	/* Get the request first */
	JANUS_VALIDATE_JSON_OBJECT(root, request_parameters,
		error_code, error_cause, TRUE,
		JANUS_VIDEOROOM_ERROR_MISSING_ELEMENT, JANUS_VIDEOROOM_ERROR_INVALID_ELEMENT);
	if(error_code != 0)
		goto plugin_response;
	json_t *request = json_object_get(root, "request");
	/* Some requests ('create', 'destroy', 'exists', 'list') can be handled synchronously */
	const char *request_text = json_string_value(request);
	if(!strcasecmp(request_text, "create")) {
		/* Create a new videoroom */
		JANUS_LOG(LOG_VERB, "Creating a new videoroom\n");
		JANUS_VALIDATE_JSON_OBJECT(root, create_parameters,
			error_code, error_cause, TRUE,
			JANUS_VIDEOROOM_ERROR_MISSING_ELEMENT, JANUS_VIDEOROOM_ERROR_INVALID_ELEMENT);
		if(error_code != 0)
			goto plugin_response;
		if(admin_key != NULL) {
			/* An admin key was specified: make sure it was provided, and that it's valid */
			JANUS_VALIDATE_JSON_OBJECT(root, adminkey_parameters,
				error_code, error_cause, TRUE,
				JANUS_VIDEOROOM_ERROR_MISSING_ELEMENT, JANUS_VIDEOROOM_ERROR_INVALID_ELEMENT);
			if(error_code != 0)
				goto plugin_response;
			JANUS_CHECK_SECRET(admin_key, root, "admin_key", error_code, error_cause,
				JANUS_VIDEOROOM_ERROR_MISSING_ELEMENT, JANUS_VIDEOROOM_ERROR_INVALID_ELEMENT, JANUS_VIDEOROOM_ERROR_UNAUTHORIZED);
			if(error_code != 0)
				goto plugin_response;
		}
		json_t *desc = json_object_get(root, "description");
		json_t *is_private = json_object_get(root, "is_private");
		json_t *req_pvtid = json_object_get(root, "require_pvtid");
		json_t *secret = json_object_get(root, "secret");
		json_t *pin = json_object_get(root, "pin");
		json_t *bitrate = json_object_get(root, "bitrate");
		json_t *fir_freq = json_object_get(root, "fir_freq");
		json_t *publishers = json_object_get(root, "publishers");
		json_t *allowed = json_object_get(root, "allowed");
		json_t *audiocodec = json_object_get(root, "audiocodec");
		if(audiocodec) {
			const char *audiocodec_value = json_string_value(audiocodec);
			gchar **list = g_strsplit(audiocodec_value, ",", 4);
			gchar *codec = list[0];
			if(codec != NULL) {
				int i=0;
				while(codec != NULL) {
					if(i == 3) {
						break;
					}
					if(strlen(codec) == 0 ||
							(strcasecmp(codec, "opus") && strcasecmp(codec, "g722") &&
							strcasecmp(codec, "isac32") && strcasecmp(codec, "isac16") &&
							strcasecmp(codec, "pcmu") && strcasecmp(codec, "pcma"))) {
						JANUS_LOG(LOG_ERR, "Invalid element (audiocodec can only be or contain opus, isac32, isac16, pcmu, pcma or g722)\n");
						error_code = JANUS_VIDEOROOM_ERROR_INVALID_ELEMENT;
						g_snprintf(error_cause, 512, "Invalid element (audiocodec can only be or contain opus, isac32, isac16, pcmu, pcma or g722)");
						goto plugin_response;
					}
					i++;
					codec = list[i];
				}
			}
			g_clear_pointer(&list, g_strfreev);
		}
		json_t *videocodec = json_object_get(root, "videocodec");
		if(videocodec) {
			const char *videocodec_value = json_string_value(videocodec);
			gchar **list = g_strsplit(videocodec_value, ",", 4);
			gchar *codec = list[0];
			if(codec != NULL) {
				int i=0;
				while(codec != NULL) {
					if(i == 3) {
						break;
					}
					if(strlen(codec) == 0 || (strcasecmp(codec, "vp8") &&
							strcasecmp(codec, "vp9") && strcasecmp(codec, "h264"))) {
						JANUS_LOG(LOG_ERR, "Invalid element (videocodec can only be or contain vp8, vp9 or h264)\n");
						error_code = JANUS_VIDEOROOM_ERROR_INVALID_ELEMENT;
						g_snprintf(error_cause, 512, "Invalid element (videocodec can only be or contain vp8, vp9 or h264)");
						goto plugin_response;
					}
					i++;
					codec = list[i];
				}
			}
			g_clear_pointer(&list, g_strfreev);
		}
		json_t *svc = json_object_get(root, "video_svc");
		json_t *audiolevel_ext = json_object_get(root, "audiolevel_ext");
		json_t *audiolevel_event = json_object_get(root, "audiolevel_event");
		json_t *audio_active_packets = json_object_get(root, "audio_active_packets");
		json_t *audio_level_average = json_object_get(root, "audio_level_average");
		json_t *videoorient_ext = json_object_get(root, "videoorient_ext");
		json_t *playoutdelay_ext = json_object_get(root, "playoutdelay_ext");
		json_t *transport_wide_cc_ext = json_object_get(root, "transport_wide_cc_ext");
		json_t *notify_joining = json_object_get(root, "notify_joining");
		json_t *record = json_object_get(root, "record");
		json_t *rec_dir = json_object_get(root, "rec_dir");
		json_t *permanent = json_object_get(root, "permanent");
		if(allowed) {
			/* Make sure the "allowed" array only contains strings */
			gboolean ok = TRUE;
			if(json_array_size(allowed) > 0) {
				size_t i = 0;
				for(i=0; i<json_array_size(allowed); i++) {
					json_t *a = json_array_get(allowed, i);
					if(!a || !json_is_string(a)) {
						ok = FALSE;
						break;
					}
				}
			}
			if(!ok) {
				JANUS_LOG(LOG_ERR, "Invalid element in the allowed array (not a string)\n");
				error_code = JANUS_VIDEOROOM_ERROR_INVALID_ELEMENT;
				g_snprintf(error_cause, 512, "Invalid element in the allowed array (not a string)");
				goto plugin_response;
			}
		}
		gboolean save = permanent ? json_is_true(permanent) : FALSE;
		if(save && config == NULL) {
			JANUS_LOG(LOG_ERR, "No configuration file, can't create permanent room\n");
			error_code = JANUS_VIDEOROOM_ERROR_UNKNOWN_ERROR;
			g_snprintf(error_cause, 512, "No configuration file, can't create permanent room");
			goto plugin_response;
		}
		guint64 room_id = 0;
		json_t *room = json_object_get(root, "room");
		if(room) {
			room_id = json_integer_value(room);
			if(room_id == 0) {
				JANUS_LOG(LOG_WARN, "Desired room ID is 0, which is not allowed... picking random ID instead\n");
			}
		}
		janus_mutex_lock(&rooms_mutex);
		if(room_id > 0) {
			/* Let's make sure the room doesn't exist already */
			if(g_hash_table_lookup(rooms, &room_id) != NULL) {
				/* It does... */
				janus_mutex_unlock(&rooms_mutex);
				JANUS_LOG(LOG_ERR, "Room %"SCNu64" already exists!\n", room_id);
				error_code = JANUS_VIDEOROOM_ERROR_ROOM_EXISTS;
				g_snprintf(error_cause, 512, "Room %"SCNu64" already exists", room_id);
				goto plugin_response;
			}
		}
		/* Create the room */
		janus_videoroom *videoroom = g_malloc0(sizeof(janus_videoroom));
		/* Generate a random ID */
		if(room_id == 0) {
			while(room_id == 0) {
				room_id = janus_random_uint64();
				if(g_hash_table_lookup(rooms, &room_id) != NULL) {
					/* Room ID already taken, try another one */
					room_id = 0;
				}
			}
		}
		videoroom->room_id = room_id;
		char *description = NULL;
		if(desc != NULL && strlen(json_string_value(desc)) > 0) {
			description = g_strdup(json_string_value(desc));
		} else {
			char roomname[255];
			g_snprintf(roomname, 255, "Room %"SCNu64"", videoroom->room_id);
			description = g_strdup(roomname);
		}
		videoroom->room_name = description;
		videoroom->is_private = is_private ? json_is_true(is_private) : FALSE;
		videoroom->require_pvtid = req_pvtid ? json_is_true(req_pvtid) : FALSE;
		if(secret)
			videoroom->room_secret = g_strdup(json_string_value(secret));
		if(pin)
			videoroom->room_pin = g_strdup(json_string_value(pin));
		videoroom->max_publishers = 3;	/* FIXME How should we choose a default? */
		if(publishers)
			videoroom->max_publishers = json_integer_value(publishers);
		if(videoroom->max_publishers < 0)
			videoroom->max_publishers = 3;	/* FIXME How should we choose a default? */
		videoroom->bitrate = 0;
		if(bitrate)
			videoroom->bitrate = json_integer_value(bitrate);
		if(videoroom->bitrate > 0 && videoroom->bitrate < 64000)
			videoroom->bitrate = 64000;	/* Don't go below 64k */
		videoroom->fir_freq = 0;
		if(fir_freq)
			videoroom->fir_freq = json_integer_value(fir_freq);
		/* By default, we force Opus as the only audio codec */
		videoroom->acodec[0] = JANUS_VIDEOROOM_OPUS;
		videoroom->acodec[1] = JANUS_VIDEOROOM_NOAUDIO;
		videoroom->acodec[2] = JANUS_VIDEOROOM_NOAUDIO;
		/* Check if we're forcing a different single codec, or allowing more than one */
		if(audiocodec) {
			const char *audiocodec_value = json_string_value(audiocodec);
			gchar **list = g_strsplit(audiocodec_value, ",", 4);
			gchar *codec = list[0];
			if(codec != NULL) {
				int i=0;
				while(codec != NULL) {
					if(i == 3) {
						JANUS_LOG(LOG_WARN, "Ignoring extra audio codecs: %s\n", codec);
						break;
					}
					if(strlen(codec) > 0)
						videoroom->acodec[i] = janus_videoroom_audiocodec_from_name(codec);
					i++;
					codec = list[i];
				}
			}
			g_clear_pointer(&list, g_strfreev);
		}
		/* By default, we force VP8 as the only video codec */
		videoroom->vcodec[0] = JANUS_VIDEOROOM_VP8;
		videoroom->vcodec[1] = JANUS_VIDEOROOM_NOVIDEO;
		videoroom->vcodec[2] = JANUS_VIDEOROOM_NOVIDEO;
		/* Check if we're forcing a different single codec, or allowing more than one */
		if(videocodec) {
			const char *videocodec_value = json_string_value(videocodec);
			gchar **list = g_strsplit(videocodec_value, ",", 4);
			gchar *codec = list[0];
			if(codec != NULL) {
				int i=0;
				while(codec != NULL) {
					if(i == 3) {
						JANUS_LOG(LOG_WARN, "Ignoring extra video codecs: %s\n", codec);
						break;
					}
					if(strlen(codec) > 0)
						videoroom->vcodec[i] = janus_videoroom_videocodec_from_name(codec);
					i++;
					codec = list[i];
				}
			}
			g_clear_pointer(&list, g_strfreev);
		}
		if(svc && json_is_true(svc)) {
			if(videoroom->vcodec[0] == JANUS_VIDEOROOM_VP9 &&
					videoroom->vcodec[1] == JANUS_VIDEOROOM_NOVIDEO &&
					videoroom->vcodec[2] == JANUS_VIDEOROOM_NOVIDEO) {
				videoroom->do_svc = TRUE;
			} else {
				JANUS_LOG(LOG_WARN, "SVC is only supported, in an experimental way, for VP9 only rooms: disabling it...\n");
			}
		}
		videoroom->audiolevel_ext = audiolevel_ext ? json_is_true(audiolevel_ext) : TRUE;
		videoroom->audiolevel_event = audiolevel_event ? json_is_true(audiolevel_event) : FALSE;
		if(videoroom->audiolevel_event) {
			videoroom->audio_active_packets = 100;
			if(json_integer_value(audio_active_packets) > 0) {
				videoroom->audio_active_packets = json_integer_value(audio_active_packets);
			} else {
				JANUS_LOG(LOG_WARN, "Invalid audio_active_packets value provided, using default: %d\n", videoroom->audio_active_packets);
			}
			videoroom->audio_level_average = 25;
			if(json_integer_value(audio_level_average) > 0) {
				videoroom->audio_level_average = json_integer_value(audio_level_average);
			} else {
				JANUS_LOG(LOG_WARN, "Invalid audio_level_average value provided, using default: %d\n", videoroom->audio_level_average);
			}
		}
		videoroom->videoorient_ext = videoorient_ext ? json_is_true(videoorient_ext) : TRUE;
		videoroom->playoutdelay_ext = playoutdelay_ext ? json_is_true(playoutdelay_ext) : TRUE;
		videoroom->transport_wide_cc_ext = transport_wide_cc_ext ? json_is_true(transport_wide_cc_ext) : FALSE;
		/* By default, the videoroom plugin does not notify about participants simply joining the room.
		   It only notifies when the participant actually starts publishing media. */
		videoroom->notify_joining = notify_joining ? json_is_true(notify_joining) : FALSE;
		if(record) {
			videoroom->record = json_is_true(record);
		}
		if(rec_dir) {
			videoroom->rec_dir = g_strdup(json_string_value(rec_dir));
		}
		g_atomic_int_set(&videoroom->destroyed, 0);
		janus_mutex_init(&videoroom->mutex);
		janus_refcount_init(&videoroom->ref, janus_videoroom_room_free);
		videoroom->participants = g_hash_table_new_full(g_int64_hash, g_int64_equal, (GDestroyNotify)g_free, NULL);
		videoroom->private_ids = g_hash_table_new(NULL, NULL);
		videoroom->allowed = g_hash_table_new_full(g_str_hash, g_str_equal, (GDestroyNotify)g_free, NULL);
		if(allowed != NULL) {
			/* Populate the "allowed" list as an ACL for people trying to join */
			if(json_array_size(allowed) > 0) {
				size_t i = 0;
				for(i=0; i<json_array_size(allowed); i++) {
					const char *token = json_string_value(json_array_get(allowed, i));
					if(!g_hash_table_lookup(videoroom->allowed, token))
						g_hash_table_insert(videoroom->allowed, g_strdup(token), GINT_TO_POINTER(TRUE));
				}
			}
			videoroom->check_allowed = TRUE;
		}
		/* Compute a list of the supported codecs for the summary */
		char audio_codecs[100], video_codecs[100];
		memset(audio_codecs, 0, sizeof(audio_codecs));
		g_snprintf(audio_codecs, sizeof(audio_codecs), "%s", janus_videoroom_audiocodec_name(videoroom->acodec[0]));
		if(videoroom->acodec[1] != JANUS_VIDEOROOM_NOAUDIO) {
			g_strlcat(audio_codecs, "|", sizeof(audio_codecs));
			g_strlcat(audio_codecs, janus_videoroom_audiocodec_name(videoroom->acodec[1]), sizeof(audio_codecs));
		}
		if(videoroom->acodec[2] != JANUS_VIDEOROOM_NOAUDIO) {
			g_strlcat(audio_codecs, "|", sizeof(audio_codecs));
			g_strlcat(audio_codecs, janus_videoroom_audiocodec_name(videoroom->acodec[2]), sizeof(audio_codecs));
		}
		memset(video_codecs, 0, sizeof(video_codecs));
		g_snprintf(video_codecs, sizeof(video_codecs), "%s", janus_videoroom_videocodec_name(videoroom->vcodec[0]));
		if(videoroom->vcodec[1] != JANUS_VIDEOROOM_NOVIDEO) {
			g_strlcat(video_codecs, "|", sizeof(video_codecs));
			g_strlcat(video_codecs, janus_videoroom_videocodec_name(videoroom->vcodec[1]), sizeof(video_codecs));
		}
		if(videoroom->vcodec[2] != JANUS_VIDEOROOM_NOVIDEO) {
			g_strlcat(video_codecs, "|", sizeof(video_codecs));
			g_strlcat(video_codecs, janus_videoroom_videocodec_name(videoroom->vcodec[2]), sizeof(video_codecs));
		}
		JANUS_LOG(LOG_VERB, "Created videoroom: %"SCNu64" (%s, %s, %s/%s codecs, secret: %s, pin: %s, pvtid: %s)\n",
			videoroom->room_id, videoroom->room_name,
			videoroom->is_private ? "private" : "public",
			audio_codecs, video_codecs,
			videoroom->room_secret ? videoroom->room_secret : "no secret",
			videoroom->room_pin ? videoroom->room_pin : "no pin",
			videoroom->require_pvtid ? "required" : "optional");
		if(videoroom->record) {
			JANUS_LOG(LOG_VERB, "  -- Room is going to be recorded in %s\n", videoroom->rec_dir ? videoroom->rec_dir : "the current folder");
		}
		if(save) {
			/* This room is permanent: save to the configuration file too
			 * FIXME: We should check if anything fails... */
			JANUS_LOG(LOG_VERB, "Saving room %"SCNu64" permanently in config file\n", videoroom->room_id);
			janus_mutex_lock(&config_mutex);
			char cat[BUFSIZ], value[BUFSIZ];
			/* The room ID is the category */
			g_snprintf(cat, BUFSIZ, "%"SCNu64, videoroom->room_id);
			janus_config_add_category(config, cat);
			/* Now for the values */
			janus_config_add_item(config, cat, "description", videoroom->room_name);
			if(videoroom->is_private)
				janus_config_add_item(config, cat, "is_private", "yes");
			if(videoroom->require_pvtid)
				janus_config_add_item(config, cat, "require_pvtid", "yes");
			g_snprintf(value, BUFSIZ, "%"SCNu32, videoroom->bitrate);
			janus_config_add_item(config, cat, "bitrate", value);
			g_snprintf(value, BUFSIZ, "%d", videoroom->max_publishers);
			janus_config_add_item(config, cat, "publishers", value);
			if(videoroom->fir_freq) {
				g_snprintf(value, BUFSIZ, "%"SCNu16, videoroom->fir_freq);
				janus_config_add_item(config, cat, "fir_freq", value);
			}
			char audio_codecs[100];
			memset(audio_codecs, 0, sizeof(audio_codecs));
			g_snprintf(audio_codecs, sizeof(audio_codecs), "%s", janus_videoroom_audiocodec_name(videoroom->acodec[0]));
			if(videoroom->acodec[1] != JANUS_VIDEOROOM_NOAUDIO) {
				g_strlcat(audio_codecs, ",", sizeof(audio_codecs));
				g_strlcat(audio_codecs, janus_videoroom_audiocodec_name(videoroom->acodec[1]), sizeof(audio_codecs));
			}
			if(videoroom->acodec[2] != JANUS_VIDEOROOM_NOAUDIO) {
				g_strlcat(audio_codecs, ",", sizeof(audio_codecs));
				g_strlcat(audio_codecs, janus_videoroom_audiocodec_name(videoroom->acodec[2]), sizeof(audio_codecs));
			}
			janus_config_add_item(config, cat, "audiocodec", audio_codecs);
			char video_codecs[100];
			memset(video_codecs, 0, sizeof(video_codecs));
			g_snprintf(video_codecs, sizeof(video_codecs), "%s", janus_videoroom_videocodec_name(videoroom->vcodec[0]));
			if(videoroom->vcodec[1] != JANUS_VIDEOROOM_NOVIDEO) {
				g_strlcat(video_codecs, ",", sizeof(video_codecs));
				g_strlcat(video_codecs, janus_videoroom_videocodec_name(videoroom->vcodec[1]), sizeof(video_codecs));
			}
			if(videoroom->vcodec[2] != JANUS_VIDEOROOM_NOVIDEO) {
				g_strlcat(video_codecs, ",", sizeof(video_codecs));
				g_strlcat(video_codecs, janus_videoroom_videocodec_name(videoroom->vcodec[2]), sizeof(video_codecs));
			}
			janus_config_add_item(config, cat, "videocodec", video_codecs);
			if(videoroom->do_svc)
				janus_config_add_item(config, cat, "video_svc", "yes");
			if(videoroom->room_secret)
				janus_config_add_item(config, cat, "secret", videoroom->room_secret);
			if(videoroom->room_pin)
				janus_config_add_item(config, cat, "pin", videoroom->room_pin);
			if(videoroom->audiolevel_ext) {
				janus_config_add_item(config, cat, "audiolevel_ext", "yes");
				if(videoroom->audiolevel_event)
					janus_config_add_item(config, cat, "audiolevel_event", "yes");
				if(videoroom->audio_active_packets > 0) {
					g_snprintf(value, BUFSIZ, "%d", videoroom->audio_active_packets);
					janus_config_add_item(config, cat, "audio_active_packets", value);
				}
				if(videoroom->audio_level_average > 0) {
					g_snprintf(value, BUFSIZ, "%d", videoroom->audio_level_average);
					janus_config_add_item(config, cat, "audio_level_average", value);
				}
			}
			if(videoroom->videoorient_ext)
				janus_config_add_item(config, cat, "videoorient_ext", "yes");
			if(videoroom->playoutdelay_ext)
				janus_config_add_item(config, cat, "playoutdelay_ext", "yes");
			if(videoroom->transport_wide_cc_ext)
				janus_config_add_item(config, cat, "transport_wide_cc_ext", "yes");
			if(videoroom->notify_joining)
				janus_config_add_item(config, cat, "notify_joining", "yes");
			if(videoroom->record)
				janus_config_add_item(config, cat, "record", "yes");
			if(videoroom->rec_dir)
				janus_config_add_item(config, cat, "rec_dir", videoroom->rec_dir);
			/* Save modified configuration */
			if(janus_config_save(config, config_folder, JANUS_VIDEOROOM_PACKAGE) < 0)
				save = FALSE;	/* This will notify the user the room is not permanent */
			janus_mutex_unlock(&config_mutex);
		}
		/* Show updated rooms list */
		GHashTableIter iter;
		gpointer value;
		g_hash_table_insert(rooms, janus_uint64_dup(videoroom->room_id), videoroom);
		g_hash_table_iter_init(&iter, rooms);
		while (g_hash_table_iter_next(&iter, NULL, &value)) {
			janus_videoroom *vr = value;
			JANUS_LOG(LOG_VERB, "  ::: [%"SCNu64"][%s] %"SCNu32", max %d publishers, FIR frequency of %d seconds\n", vr->room_id, vr->room_name, vr->bitrate, vr->max_publishers, vr->fir_freq);
		}
		janus_mutex_unlock(&rooms_mutex);
		/* Send info back */
		response = json_object();
		json_object_set_new(response, "videoroom", json_string("created"));
		json_object_set_new(response, "room", json_integer(videoroom->room_id));
		json_object_set_new(response, "permanent", save ? json_true() : json_false());
		/* Also notify event handlers */
		if(notify_events && gateway->events_is_enabled()) {
			json_t *info = json_object();
			json_object_set_new(info, "event", json_string("created"));
			json_object_set_new(info, "room", json_integer(videoroom->room_id));
			gateway->notify_event(&janus_videoroom_plugin, session->handle, info);
		}
		goto plugin_response;
	} else if(!strcasecmp(request_text, "edit")) {
		/* Edit the properties for an existing videoroom */
		JANUS_LOG(LOG_VERB, "Attempt to edit the properties of an existing videoroom room\n");
		JANUS_VALIDATE_JSON_OBJECT(root, edit_parameters,
			error_code, error_cause, TRUE,
			JANUS_VIDEOROOM_ERROR_MISSING_ELEMENT, JANUS_VIDEOROOM_ERROR_INVALID_ELEMENT);
		if(error_code != 0)
			goto plugin_response;
		/* We only allow for a limited set of properties to be edited */
		json_t *desc = json_object_get(root, "new_description");
		json_t *is_private = json_object_get(root, "new_is_private");
		json_t *req_pvtid = json_object_get(root, "new_require_pvtid");
		json_t *secret = json_object_get(root, "new_secret");
		json_t *pin = json_object_get(root, "new_pin");
		json_t *bitrate = json_object_get(root, "new_bitrate");
		json_t *fir_freq = json_object_get(root, "new_fir_freq");
		json_t *publishers = json_object_get(root, "new_publishers");
		json_t *permanent = json_object_get(root, "permanent");
		gboolean save = permanent ? json_is_true(permanent) : FALSE;
		if(save && config == NULL) {
			JANUS_LOG(LOG_ERR, "No configuration file, can't edit room permanently\n");
			error_code = JANUS_VIDEOROOM_ERROR_UNKNOWN_ERROR;
			g_snprintf(error_cause, 512, "No configuration file, can't edit room permanently");
			goto plugin_response;
		}
		janus_mutex_lock(&rooms_mutex);
		janus_videoroom *videoroom = NULL;
		error_code = janus_videoroom_access_room(root, TRUE, FALSE, &videoroom, error_cause, sizeof(error_cause));
		if(error_code != 0) {
			janus_mutex_unlock(&rooms_mutex);
			goto plugin_response;
		}
		/* Edit the room properties that were provided */
		if(desc != NULL && strlen(json_string_value(desc)) > 0) {
			char *old_description = videoroom->room_name;
			char *new_description = g_strdup(json_string_value(desc));
			videoroom->room_name = new_description;
			g_free(old_description);
		}
		if(is_private)
			videoroom->is_private = json_is_true(is_private);
		if(req_pvtid)
			videoroom->require_pvtid = json_is_true(req_pvtid);
		if(publishers)
			videoroom->max_publishers = json_integer_value(publishers);
		if(bitrate) {
			videoroom->bitrate = json_integer_value(bitrate);
			if(videoroom->bitrate > 0 && videoroom->bitrate < 64000)
				videoroom->bitrate = 64000;	/* Don't go below 64k */
		}
		if(fir_freq)
			videoroom->fir_freq = json_integer_value(fir_freq);
		if(secret && strlen(json_string_value(secret)) > 0) {
			char *old_secret = videoroom->room_secret;
			char *new_secret = g_strdup(json_string_value(secret));
			videoroom->room_secret = new_secret;
			g_free(old_secret);
		}
		if(pin && strlen(json_string_value(pin)) > 0) {
			char *old_pin = videoroom->room_pin;
			char *new_pin = g_strdup(json_string_value(pin));
			videoroom->room_pin = new_pin;
			g_free(old_pin);
		}
		if(save) {
			/* This room is permanent: save to the configuration file too
			 * FIXME: We should check if anything fails... */
			JANUS_LOG(LOG_VERB, "Modifying room %"SCNu64" permanently in config file\n", videoroom->room_id);
			janus_mutex_lock(&config_mutex);
			char cat[BUFSIZ], value[BUFSIZ];
			/* The room ID is the category */
			g_snprintf(cat, BUFSIZ, "%"SCNu64, videoroom->room_id);
			/* Remove the old category first */
			janus_config_remove_category(config, cat);
			/* Now write the room details again */
			janus_config_add_category(config, cat);
			janus_config_add_item(config, cat, "description", videoroom->room_name);
			if(videoroom->is_private)
				janus_config_add_item(config, cat, "is_private", "yes");
			if(videoroom->require_pvtid)
				janus_config_add_item(config, cat, "require_pvtid", "yes");
			g_snprintf(value, BUFSIZ, "%"SCNu32, videoroom->bitrate);
			janus_config_add_item(config, cat, "bitrate", value);
			g_snprintf(value, BUFSIZ, "%d", videoroom->max_publishers);
			janus_config_add_item(config, cat, "publishers", value);
			if(videoroom->fir_freq) {
				g_snprintf(value, BUFSIZ, "%"SCNu16, videoroom->fir_freq);
				janus_config_add_item(config, cat, "fir_freq", value);
			}
			char audio_codecs[100];
			memset(audio_codecs, 0, sizeof(audio_codecs));
			g_snprintf(audio_codecs, sizeof(audio_codecs), "%s", janus_videoroom_audiocodec_name(videoroom->acodec[0]));
			if(videoroom->acodec[1] != JANUS_VIDEOROOM_NOAUDIO) {
				g_strlcat(audio_codecs, ",", sizeof(audio_codecs));
				g_strlcat(audio_codecs, janus_videoroom_audiocodec_name(videoroom->acodec[1]), sizeof(audio_codecs));
			}
			if(videoroom->acodec[2] != JANUS_VIDEOROOM_NOAUDIO) {
				g_strlcat(audio_codecs, ",", sizeof(audio_codecs));
				g_strlcat(audio_codecs, janus_videoroom_audiocodec_name(videoroom->acodec[2]), sizeof(audio_codecs));
			}
			janus_config_add_item(config, cat, "audiocodec", audio_codecs);
			char video_codecs[100];
			memset(video_codecs, 0, sizeof(video_codecs));
			g_snprintf(video_codecs, sizeof(video_codecs), "%s", janus_videoroom_videocodec_name(videoroom->vcodec[0]));
			if(videoroom->vcodec[1] != JANUS_VIDEOROOM_NOVIDEO) {
				g_strlcat(video_codecs, ",", sizeof(video_codecs));
				g_strlcat(video_codecs, janus_videoroom_videocodec_name(videoroom->vcodec[1]), sizeof(video_codecs));
			}
			if(videoroom->vcodec[2] != JANUS_VIDEOROOM_NOVIDEO) {
				g_strlcat(video_codecs, ",", sizeof(video_codecs));
				g_strlcat(video_codecs, janus_videoroom_videocodec_name(videoroom->vcodec[2]), sizeof(video_codecs));
			}
			janus_config_add_item(config, cat, "videocodec", video_codecs);
			if(videoroom->do_svc)
				janus_config_add_item(config, cat, "video_svc", "yes");
			if(videoroom->room_secret)
				janus_config_add_item(config, cat, "secret", videoroom->room_secret);
			if(videoroom->room_pin)
				janus_config_add_item(config, cat, "pin", videoroom->room_pin);
			if(videoroom->record)
				janus_config_add_item(config, cat, "record", "yes");
			if(videoroom->rec_dir)
				janus_config_add_item(config, cat, "rec_dir", videoroom->rec_dir);
			/* Save modified configuration */
			if(janus_config_save(config, config_folder, JANUS_VIDEOROOM_PACKAGE) < 0)
				save = FALSE;	/* This will notify the user the room changes are not permanent */
			janus_mutex_unlock(&config_mutex);
		}
		janus_mutex_unlock(&rooms_mutex);
		/* Send info back */
		response = json_object();
		json_object_set_new(response, "videoroom", json_string("edited"));
		json_object_set_new(response, "room", json_integer(videoroom->room_id));
		json_object_set_new(response, "permanent", save ? json_true() : json_false());
		/* Also notify event handlers */
		if(notify_events && gateway->events_is_enabled()) {
			json_t *info = json_object();
			json_object_set_new(info, "event", json_string("edited"));
			json_object_set_new(info, "room", json_integer(videoroom->room_id));
			gateway->notify_event(&janus_videoroom_plugin, session->handle, info);
		}
		goto plugin_response;
	} else if(!strcasecmp(request_text, "destroy")) {
		JANUS_LOG(LOG_VERB, "Attempt to destroy an existing videoroom room\n");
		JANUS_VALIDATE_JSON_OBJECT(root, destroy_parameters,
			error_code, error_cause, TRUE,
			JANUS_VIDEOROOM_ERROR_MISSING_ELEMENT, JANUS_VIDEOROOM_ERROR_INVALID_ELEMENT);
		if(error_code != 0)
			goto plugin_response;
		json_t *room = json_object_get(root, "room");
		json_t *permanent = json_object_get(root, "permanent");
		gboolean save = permanent ? json_is_true(permanent) : FALSE;
		if(save && config == NULL) {
			JANUS_LOG(LOG_ERR, "No configuration file, can't destroy room permanently\n");
			error_code = JANUS_VIDEOROOM_ERROR_UNKNOWN_ERROR;
			g_snprintf(error_cause, 512, "No configuration file, can't destroy room permanently");
			goto plugin_response;
		}
		guint64 room_id = json_integer_value(room);
		janus_mutex_lock(&rooms_mutex);
		janus_videoroom *videoroom = NULL;
		error_code = janus_videoroom_access_room(root, TRUE, FALSE, &videoroom, error_cause, sizeof(error_cause));
		if(error_code != 0) {
			janus_mutex_unlock(&rooms_mutex);
			goto plugin_response;
		}
		/* Remove room, but add a reference until we're done */
		janus_refcount_increase(&videoroom->ref);
		g_hash_table_remove(rooms, &room_id);
		/* Notify all participants that the fun is over, and that they'll be kicked */
		JANUS_LOG(LOG_VERB, "Notifying all participants\n");
		json_t *destroyed = json_object();
		json_object_set_new(destroyed, "videoroom", json_string("destroyed"));
		json_object_set_new(destroyed, "room", json_integer(room_id));
		GHashTableIter iter;
		gpointer value;
		janus_mutex_lock(&videoroom->mutex);
		g_hash_table_iter_init(&iter, videoroom->participants);
		while (g_hash_table_iter_next(&iter, NULL, &value)) {
			janus_videoroom_publisher *p = value;
			if(p && p->session) {
				g_clear_pointer(&p->room, janus_videoroom_room_dereference);
				/* Notify the user we're going to destroy the room... */
				int ret = gateway->push_event(p->session->handle, &janus_videoroom_plugin, NULL, destroyed, NULL);
				JANUS_LOG(LOG_VERB, "  >> %d (%s)\n", ret, janus_get_api_error(ret));
				/* ... and then ask the core to close the PeerConnection */
				gateway->close_pc(p->session->handle);
			}
		}
		json_decref(destroyed);
		janus_mutex_unlock(&videoroom->mutex);
		/* Also notify event handlers */
		if(notify_events && gateway->events_is_enabled()) {
			json_t *info = json_object();
			json_object_set_new(info, "event", json_string("destroyed"));
			json_object_set_new(info, "room", json_integer(room_id));
			gateway->notify_event(&janus_videoroom_plugin, session->handle, info);
		}
		janus_mutex_unlock(&rooms_mutex);
		if(save) {
			/* This change is permanent: save to the configuration file too
			 * FIXME: We should check if anything fails... */
			JANUS_LOG(LOG_VERB, "Destroying room %"SCNu64" permanently in config file\n", room_id);
			janus_mutex_lock(&config_mutex);
			char cat[BUFSIZ];
			/* The room ID is the category */
			g_snprintf(cat, BUFSIZ, "%"SCNu64, room_id);
			janus_config_remove_category(config, cat);
			/* Save modified configuration */
			if(janus_config_save(config, config_folder, JANUS_VIDEOROOM_PACKAGE) < 0)
				save = FALSE;	/* This will notify the user the room destruction is not permanent */
			janus_mutex_unlock(&config_mutex);
		}
		janus_refcount_decrease(&videoroom->ref);
		/* Done */
		response = json_object();
		json_object_set_new(response, "videoroom", json_string("destroyed"));
		json_object_set_new(response, "room", json_integer(room_id));
		json_object_set_new(response, "permanent", save ? json_true() : json_false());
		goto plugin_response;
	} else if(!strcasecmp(request_text, "list")) {
		/* List all rooms (but private ones) and their details (except for the secret, of course...) */
		json_t *list = json_array();
		JANUS_LOG(LOG_VERB, "Getting the list of video rooms\n");
		janus_mutex_lock(&rooms_mutex);
		GHashTableIter iter;
		gpointer value;
		g_hash_table_iter_init(&iter, rooms);
		while(g_hash_table_iter_next(&iter, NULL, &value)) {
			janus_videoroom *room = value;
			if(!room)
				continue;
			janus_refcount_increase(&room->ref);
			if(room->is_private) {
				/* Skip private room */
				JANUS_LOG(LOG_VERB, "Skipping private room '%s'\n", room->room_name);
				janus_refcount_decrease(&room->ref);
				continue;
			}
			if(!g_atomic_int_get(&room->destroyed)) {
				json_t *rl = json_object();
				json_object_set_new(rl, "room", json_integer(room->room_id));
				json_object_set_new(rl, "description", json_string(room->room_name));
				json_object_set_new(rl, "pin_required", room->room_pin ? json_true() : json_false());
				json_object_set_new(rl, "max_publishers", json_integer(room->max_publishers));
				json_object_set_new(rl, "bitrate", json_integer(room->bitrate));
				json_object_set_new(rl, "fir_freq", json_integer(room->fir_freq));
				char audio_codecs[100];
				memset(audio_codecs, 0, sizeof(audio_codecs));
				g_snprintf(audio_codecs, sizeof(audio_codecs), "%s", janus_videoroom_audiocodec_name(room->acodec[0]));
				if(room->acodec[1] != JANUS_VIDEOROOM_NOAUDIO) {
					g_strlcat(audio_codecs, ",", sizeof(audio_codecs));
					g_strlcat(audio_codecs, janus_videoroom_audiocodec_name(room->acodec[1]), sizeof(audio_codecs));
				}
				if(room->acodec[2] != JANUS_VIDEOROOM_NOAUDIO) {
					g_strlcat(audio_codecs, ",", sizeof(audio_codecs));
					g_strlcat(audio_codecs, janus_videoroom_audiocodec_name(room->acodec[2]), sizeof(audio_codecs));
				}
				json_object_set_new(rl, "audiocodec", json_string(audio_codecs));
				char video_codecs[100];
				memset(video_codecs, 0, sizeof(video_codecs));
				g_snprintf(video_codecs, sizeof(video_codecs), "%s", janus_videoroom_videocodec_name(room->vcodec[0]));
				if(room->vcodec[1] != JANUS_VIDEOROOM_NOVIDEO) {
					g_strlcat(video_codecs, ",", sizeof(video_codecs));
					g_strlcat(video_codecs, janus_videoroom_videocodec_name(room->vcodec[1]), sizeof(video_codecs));
				}
				if(room->vcodec[2] != JANUS_VIDEOROOM_NOVIDEO) {
					g_strlcat(video_codecs, ",", sizeof(video_codecs));
					g_strlcat(video_codecs, janus_videoroom_videocodec_name(room->vcodec[2]), sizeof(video_codecs));
				}
				json_object_set_new(rl, "videocodec", json_string(video_codecs));
				if(room->do_svc)
					json_object_set_new(rl, "video_svc", json_true());
				json_object_set_new(rl, "record", room->record ? json_true() : json_false());
				json_object_set_new(rl, "rec_dir", json_string(room->rec_dir));
				/* TODO: Should we list participants as well? or should there be a separate API call on a specific room for this? */
				json_object_set_new(rl, "num_participants", json_integer(g_hash_table_size(room->participants)));
				json_array_append_new(list, rl);
			}
			janus_refcount_decrease(&room->ref);
		}
		janus_mutex_unlock(&rooms_mutex);
		response = json_object();
		json_object_set_new(response, "videoroom", json_string("success"));
		json_object_set_new(response, "list", list);
		goto plugin_response;
	} else if(!strcasecmp(request_text, "rtp_forward")) {
		JANUS_VALIDATE_JSON_OBJECT(root, rtp_forward_parameters,
			error_code, error_cause, TRUE,
			JANUS_VIDEOROOM_ERROR_MISSING_ELEMENT, JANUS_VIDEOROOM_ERROR_INVALID_ELEMENT);
		if(error_code != 0)
			goto plugin_response;
		json_t *room = json_object_get(root, "room");
		json_t *pub_id = json_object_get(root, "publisher_id");
		int video_port[3] = {-1, -1, -1}, video_pt[3] = {0, 0, 0};
		uint32_t video_ssrc[3] = {0, 0, 0};
		int audio_port = -1, audio_pt = 0;
		uint32_t audio_ssrc = 0;
		int data_port = -1;
		int srtp_suite = 0;
		const char *srtp_crypto = NULL;
		/* There may be multiple target video ports (e.g., publisher simulcasting) */
		json_t *vid_port = json_object_get(root, "video_port");
		if(vid_port) {
			video_port[0] = json_integer_value(vid_port);
			json_t *pt = json_object_get(root, "video_pt");
			if(pt)
				video_pt[0] = json_integer_value(pt);
			json_t *ssrc = json_object_get(root, "video_ssrc");
			if(ssrc)
				video_ssrc[0] = json_integer_value(ssrc);
		}
		vid_port = json_object_get(root, "video_port_2");
		if(vid_port) {
			video_port[1] = json_integer_value(vid_port);
			json_t *pt = json_object_get(root, "video_pt_2");
			if(pt)
				video_pt[1] = json_integer_value(pt);
			json_t *ssrc = json_object_get(root, "video_ssrc_2");
			if(ssrc)
				video_ssrc[1] = json_integer_value(ssrc);
		}
		vid_port = json_object_get(root, "video_port_3");
		if(vid_port) {
			video_port[2] = json_integer_value(vid_port);
			json_t *pt = json_object_get(root, "video_pt_3");
			if(pt)
				video_pt[2] = json_integer_value(pt);
			json_t *ssrc = json_object_get(root, "video_ssrc_3");
			if(ssrc)
				video_ssrc[2] = json_integer_value(ssrc);
		}
		/* Audio target */
		json_t *au_port = json_object_get(root, "audio_port");
		if(au_port) {
			audio_port = json_integer_value(au_port);
			json_t *pt = json_object_get(root, "audio_pt");
			if(pt)
				audio_pt = json_integer_value(pt);
			json_t *ssrc = json_object_get(root, "audio_ssrc");
			if(ssrc)
				audio_ssrc = json_integer_value(ssrc);
		}
		/* Data target */
		json_t *d_port = json_object_get(root, "data_port");
		if(d_port) {
			data_port = json_integer_value(d_port);
		}
		json_t *json_host = json_object_get(root, "host");
		/* Besides, we may need to SRTP-encrypt this stream */
		json_t *s_suite = json_object_get(root, "srtp_suite");
		json_t *s_crypto = json_object_get(root, "srtp_crypto");
		if(s_suite && s_crypto) {
			srtp_suite = json_integer_value(s_suite);
			if(srtp_suite != 32 && srtp_suite != 80) {
				JANUS_LOG(LOG_ERR, "Invalid SRTP suite (%d)\n", srtp_suite);
				error_code = JANUS_VIDEOROOM_ERROR_INVALID_ELEMENT;
				g_snprintf(error_cause, 512, "Invalid SRTP suite (%d)", srtp_suite);
				goto plugin_response;
			}
			srtp_crypto = json_string_value(s_crypto);
		}
		guint64 room_id = json_integer_value(room);
		guint64 publisher_id = json_integer_value(pub_id);
		const char *host = json_string_value(json_host);
		janus_mutex_lock(&rooms_mutex);
		janus_videoroom *videoroom = NULL;
		error_code = janus_videoroom_access_room(root, TRUE, FALSE, &videoroom, error_cause, sizeof(error_cause));
		janus_mutex_unlock(&rooms_mutex);
		if(error_code != 0)
			goto plugin_response;
		janus_refcount_increase(&videoroom->ref);
		janus_mutex_lock(&videoroom->mutex);
		janus_videoroom_publisher *publisher = g_hash_table_lookup(videoroom->participants, &publisher_id);
		if(publisher == NULL) {
			janus_mutex_unlock(&videoroom->mutex);
			janus_refcount_decrease(&videoroom->ref);
			JANUS_LOG(LOG_ERR, "No such publisher (%"SCNu64")\n", publisher_id);
			error_code = JANUS_VIDEOROOM_ERROR_NO_SUCH_FEED;
			g_snprintf(error_cause, 512, "No such feed (%"SCNu64")", publisher_id);
			goto plugin_response;
		}
		janus_refcount_increase(&publisher->ref);	/* This is just to handle the request for now */
		if(publisher->udp_sock <= 0) {
			publisher->udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
			if(publisher->udp_sock <= 0) {
				janus_refcount_decrease(&publisher->ref);
				janus_mutex_unlock(&videoroom->mutex);
				janus_refcount_decrease(&videoroom->ref);
				JANUS_LOG(LOG_ERR, "Could not open UDP socket for rtp stream for publisher (%"SCNu64")\n", publisher_id);
				error_code = JANUS_VIDEOROOM_ERROR_UNKNOWN_ERROR;
				g_snprintf(error_cause, 512, "Could not open UDP socket for rtp stream");
				goto plugin_response;
			}
		}
		guint32 audio_handle = 0;
		guint32 video_handle[3] = {0, 0, 0};
		guint32 data_handle = 0;
		if(audio_port > 0) {
			audio_handle = janus_videoroom_rtp_forwarder_add_helper(publisher, host, audio_port, audio_pt, audio_ssrc,
				srtp_suite, srtp_crypto, 0, FALSE, FALSE);
		}
		if(video_port[0] > 0) {
			video_handle[0] = janus_videoroom_rtp_forwarder_add_helper(publisher, host, video_port[0], video_pt[0], video_ssrc[0],
				srtp_suite, srtp_crypto, 0, TRUE, FALSE);
		}
		if(video_port[1] > 0) {
			video_handle[1] = janus_videoroom_rtp_forwarder_add_helper(publisher, host, video_port[1], video_pt[1], video_ssrc[1],
				srtp_suite, srtp_crypto, 1, TRUE, FALSE);
		}
		if(video_port[2] > 0) {
			video_handle[2] = janus_videoroom_rtp_forwarder_add_helper(publisher, host, video_port[2], video_pt[2], video_ssrc[2],
				srtp_suite, srtp_crypto, 2, TRUE, FALSE);
		}
		if(data_port > 0) {
			data_handle = janus_videoroom_rtp_forwarder_add_helper(publisher, host, data_port, 0, 0, 0, NULL, 0, FALSE, TRUE);
		}
		janus_mutex_unlock(&videoroom->mutex);
		response = json_object();
		json_t *rtp_stream = json_object();
		if(audio_handle > 0) {
			json_object_set_new(rtp_stream, "audio_stream_id", json_integer(audio_handle));
			json_object_set_new(rtp_stream, "audio", json_integer(audio_port));
		}
		if(video_handle[0] > 0 || video_handle[1] > 0 || video_handle[2] > 0) {
			/* Send a FIR to the new RTP forward publisher */
			char buf[20];
			janus_rtcp_fir((char *)&buf, 20, &publisher->fir_seq);
			JANUS_LOG(LOG_VERB, "New RTP forward publisher, sending FIR to %"SCNu64" (%s)\n", publisher->user_id, publisher->display ? publisher->display : "??");
			gateway->relay_rtcp(publisher->session->handle, 1, buf, 20);
			/* Send a PLI too, just in case... */
			janus_rtcp_pli((char *)&buf, 12);
			JANUS_LOG(LOG_VERB, "New RTP forward publisher, sending PLI to %"SCNu64" (%s)\n", publisher->user_id, publisher->display ? publisher->display : "??");
			gateway->relay_rtcp(publisher->session->handle, 1, buf, 12);
			/* Update the time of when we last sent a keyframe request */
			publisher->fir_latest = janus_get_monotonic_time();
			/* Done */
			if(video_handle[0] > 0) {
				json_object_set_new(rtp_stream, "video_stream_id", json_integer(video_handle[0]));
				json_object_set_new(rtp_stream, "video", json_integer(video_port[0]));
			}
			if(video_handle[1] > 0) {
				json_object_set_new(rtp_stream, "video_stream_id_2", json_integer(video_handle[1]));
				json_object_set_new(rtp_stream, "video_2", json_integer(video_port[1]));
			}
			if(video_handle[2] > 0) {
				json_object_set_new(rtp_stream, "video_stream_id_3", json_integer(video_handle[2]));
				json_object_set_new(rtp_stream, "video_3", json_integer(video_port[2]));
			}
		}
		if(data_handle > 0) {
			json_object_set_new(rtp_stream, "data_stream_id", json_integer(data_handle));
			json_object_set_new(rtp_stream, "data", json_integer(data_port));
		}
		/* These two unrefs are related to the message handling */
		janus_refcount_decrease(&publisher->ref);
		janus_refcount_decrease(&videoroom->ref);
		json_object_set_new(rtp_stream, "host", json_string(host));
		json_object_set_new(response, "publisher_id", json_integer(publisher_id));
		json_object_set_new(response, "rtp_stream", rtp_stream);
		json_object_set_new(response, "room", json_integer(room_id));
		json_object_set_new(response, "videoroom", json_string("rtp_forward"));
		goto plugin_response;
	} else if(!strcasecmp(request_text, "stop_rtp_forward")) {
		JANUS_VALIDATE_JSON_OBJECT(root, stop_rtp_forward_parameters,
			error_code, error_cause, TRUE,
			JANUS_VIDEOROOM_ERROR_MISSING_ELEMENT, JANUS_VIDEOROOM_ERROR_INVALID_ELEMENT);
		if(error_code != 0)
			goto plugin_response;
		json_t *room = json_object_get(root, "room");
		json_t *pub_id = json_object_get(root, "publisher_id");
		json_t *id = json_object_get(root, "stream_id");

		guint64 room_id = json_integer_value(room);
		guint64 publisher_id = json_integer_value(pub_id);
		guint32 stream_id = json_integer_value(id);
		janus_mutex_lock(&rooms_mutex);
		janus_videoroom *videoroom = NULL;
		error_code = janus_videoroom_access_room(root, TRUE, FALSE, &videoroom, error_cause, sizeof(error_cause));
		janus_mutex_unlock(&rooms_mutex);
		if(error_code != 0)
			goto plugin_response;
		janus_mutex_lock(&videoroom->mutex);
		janus_refcount_increase(&videoroom->ref);
		janus_videoroom_publisher *publisher = g_hash_table_lookup(videoroom->participants, &publisher_id);
		if(publisher == NULL) {
			janus_mutex_unlock(&videoroom->mutex);
			janus_refcount_decrease(&videoroom->ref);
			JANUS_LOG(LOG_ERR, "No such publisher (%"SCNu64")\n", publisher_id);
			error_code = JANUS_VIDEOROOM_ERROR_NO_SUCH_FEED;
			g_snprintf(error_cause, 512, "No such feed (%"SCNu64")", publisher_id);
			goto plugin_response;
		}
		janus_refcount_increase(&publisher->ref);	/* Just to handle the message now */
		janus_mutex_lock(&publisher->rtp_forwarders_mutex);
		if(!g_hash_table_remove(publisher->rtp_forwarders, GUINT_TO_POINTER(stream_id))) {
			janus_mutex_unlock(&publisher->rtp_forwarders_mutex);
			janus_refcount_decrease(&publisher->ref);
			janus_mutex_unlock(&videoroom->mutex);
			janus_refcount_decrease(&videoroom->ref);
			JANUS_LOG(LOG_ERR, "No such stream (%"SCNu32")\n", stream_id);
			error_code = JANUS_VIDEOROOM_ERROR_NO_SUCH_FEED;
			g_snprintf(error_cause, 512, "No such stream (%"SCNu32")", stream_id);
			goto plugin_response;
		}
		janus_mutex_unlock(&publisher->rtp_forwarders_mutex);
		janus_refcount_decrease(&publisher->ref);
		janus_mutex_unlock(&videoroom->mutex);
		janus_refcount_decrease(&videoroom->ref);
		response = json_object();
		json_object_set_new(response, "videoroom", json_string("stop_rtp_forward"));
		json_object_set_new(response, "room", json_integer(room_id));
		json_object_set_new(response, "publisher_id", json_integer(publisher_id));
		json_object_set_new(response, "stream_id", json_integer(stream_id));
		goto plugin_response;
	} else if(!strcasecmp(request_text, "exists")) {
		/* Check whether a given room exists or not, returns true/false */	
		JANUS_VALIDATE_JSON_OBJECT(root, room_parameters,
			error_code, error_cause, TRUE,
			JANUS_VIDEOROOM_ERROR_MISSING_ELEMENT, JANUS_VIDEOROOM_ERROR_INVALID_ELEMENT);
		if(error_code != 0)
			goto plugin_response;
		json_t *room = json_object_get(root, "room");
		guint64 room_id = json_integer_value(room);
		janus_mutex_lock(&rooms_mutex);
		gboolean room_exists = g_hash_table_contains(rooms, &room_id);
		janus_mutex_unlock(&rooms_mutex);
		response = json_object();
		json_object_set_new(response, "videoroom", json_string("success"));
		json_object_set_new(response, "room", json_integer(room_id));
		json_object_set_new(response, "exists", room_exists ? json_true() : json_false());
		goto plugin_response;
	} else if(!strcasecmp(request_text, "allowed")) {
		JANUS_LOG(LOG_VERB, "Attempt to edit the list of allowed participants in an existing videoroom room\n");
		JANUS_VALIDATE_JSON_OBJECT(root, allowed_parameters,
			error_code, error_cause, TRUE,
			JANUS_VIDEOROOM_ERROR_MISSING_ELEMENT, JANUS_VIDEOROOM_ERROR_INVALID_ELEMENT);
		if(error_code != 0)
			goto plugin_response;
		json_t *action = json_object_get(root, "action");
		json_t *room = json_object_get(root, "room");
		json_t *allowed = json_object_get(root, "allowed");
		const char *action_text = json_string_value(action);
		if(strcasecmp(action_text, "enable") && strcasecmp(action_text, "disable") &&
				strcasecmp(action_text, "add") && strcasecmp(action_text, "remove")) {
			JANUS_LOG(LOG_ERR, "Unsupported action '%s' (allowed)\n", action_text);
			error_code = JANUS_VIDEOROOM_ERROR_INVALID_ELEMENT;
			g_snprintf(error_cause, 512, "Unsupported action '%s' (allowed)", action_text);
			goto plugin_response;
		}
		guint64 room_id = json_integer_value(room);
		janus_mutex_lock(&rooms_mutex);
		janus_videoroom *videoroom = NULL;
		error_code = janus_videoroom_access_room(root, TRUE, FALSE, &videoroom, error_cause, sizeof(error_cause));
		if(error_code != 0) {
			janus_mutex_unlock(&rooms_mutex);
			goto plugin_response;
		}
		janus_refcount_increase(&videoroom->ref);
		janus_mutex_unlock(&rooms_mutex);
		/* A secret may be required for this action */
		JANUS_CHECK_SECRET(videoroom->room_secret, root, "secret", error_code, error_cause,
			JANUS_VIDEOROOM_ERROR_MISSING_ELEMENT, JANUS_VIDEOROOM_ERROR_INVALID_ELEMENT, JANUS_VIDEOROOM_ERROR_UNAUTHORIZED);
		if(error_code != 0) {
			janus_refcount_decrease(&videoroom->ref);
			goto plugin_response;
		}
		if(!strcasecmp(action_text, "enable")) {
			JANUS_LOG(LOG_VERB, "Enabling the check on allowed authorization tokens for room %"SCNu64"\n", room_id);
			videoroom->check_allowed = TRUE;
		} else if(!strcasecmp(action_text, "disable")) {
			JANUS_LOG(LOG_VERB, "Disabling the check on allowed authorization tokens for room %"SCNu64" (free entry)\n", room_id);
			videoroom->check_allowed = FALSE;
		} else {
			gboolean add = !strcasecmp(action_text, "add");
			if(allowed) {
				/* Make sure the "allowed" array only contains strings */
				gboolean ok = TRUE;
				if(json_array_size(allowed) > 0) {
					size_t i = 0;
					for(i=0; i<json_array_size(allowed); i++) {
						json_t *a = json_array_get(allowed, i);
						if(!a || !json_is_string(a)) {
							ok = FALSE;
							break;
						}
					}
				}
				if(!ok) {
					JANUS_LOG(LOG_ERR, "Invalid element in the allowed array (not a string)\n");
					error_code = JANUS_VIDEOROOM_ERROR_INVALID_ELEMENT;
					g_snprintf(error_cause, 512, "Invalid element in the allowed array (not a string)");
					janus_refcount_decrease(&videoroom->ref);
					goto plugin_response;
				}
				size_t i = 0;
				for(i=0; i<json_array_size(allowed); i++) {
					const char *token = json_string_value(json_array_get(allowed, i));
					if(add) {
						if(!g_hash_table_lookup(videoroom->allowed, token))
							g_hash_table_insert(videoroom->allowed, g_strdup(token), GINT_TO_POINTER(TRUE));
					} else {
						g_hash_table_remove(videoroom->allowed, token);
					}
				}
			}
		}
		/* Prepare response */
		response = json_object();
		json_object_set_new(response, "videoroom", json_string("success"));
		json_object_set_new(response, "room", json_integer(videoroom->room_id));
		json_t *list = json_array();
		if(strcasecmp(action_text, "disable")) {
			if(g_hash_table_size(videoroom->allowed) > 0) {
				GHashTableIter iter;
				gpointer key;
				g_hash_table_iter_init(&iter, videoroom->allowed);
				while(g_hash_table_iter_next(&iter, &key, NULL)) {
					char *token = key;
					json_array_append_new(list, json_string(token));
				}
			}
			json_object_set_new(response, "allowed", list);
		}
		/* Done */
		janus_refcount_decrease(&videoroom->ref);
		JANUS_LOG(LOG_VERB, "VideoRoom room allowed list updated\n");
		goto plugin_response;
	} else if(!strcasecmp(request_text, "kick")) {
		JANUS_LOG(LOG_VERB, "Attempt to kick a participant from an existing videoroom room\n");
		JANUS_VALIDATE_JSON_OBJECT(root, kick_parameters,
			error_code, error_cause, TRUE,
			JANUS_VIDEOROOM_ERROR_MISSING_ELEMENT, JANUS_VIDEOROOM_ERROR_INVALID_ELEMENT);
		if(error_code != 0)
			goto plugin_response;
		json_t *room = json_object_get(root, "room");
		json_t *id = json_object_get(root, "id");
		guint64 room_id = json_integer_value(room);
		janus_mutex_lock(&rooms_mutex);
		janus_videoroom *videoroom = NULL;
		error_code = janus_videoroom_access_room(root, TRUE, FALSE, &videoroom, error_cause, sizeof(error_cause));
		if(error_code != 0) {
			janus_mutex_unlock(&rooms_mutex);
			goto plugin_response;
		}
		janus_refcount_increase(&videoroom->ref);
		janus_mutex_lock(&videoroom->mutex);
		janus_mutex_unlock(&rooms_mutex);
		/* A secret may be required for this action */
		JANUS_CHECK_SECRET(videoroom->room_secret, root, "secret", error_code, error_cause,
			JANUS_VIDEOROOM_ERROR_MISSING_ELEMENT, JANUS_VIDEOROOM_ERROR_INVALID_ELEMENT, JANUS_VIDEOROOM_ERROR_UNAUTHORIZED);
		if(error_code != 0) {
			janus_mutex_unlock(&videoroom->mutex);
			janus_refcount_decrease(&videoroom->ref);
			goto plugin_response;
		}
		guint64 user_id = json_integer_value(id);
		janus_videoroom_publisher *participant = g_hash_table_lookup(videoroom->participants, &user_id);
		if(participant == NULL) {
			janus_mutex_unlock(&videoroom->mutex);
			janus_refcount_decrease(&videoroom->ref);
			JANUS_LOG(LOG_ERR, "No such user %"SCNu64" in room %"SCNu64"\n", user_id, room_id);
			error_code = JANUS_VIDEOROOM_ERROR_NO_SUCH_FEED;
			g_snprintf(error_cause, 512, "No such user %"SCNu64" in room %"SCNu64, user_id, room_id);
			goto plugin_response;
		}
		if(participant->kicked) {
			/* Already kicked */
			janus_mutex_unlock(&videoroom->mutex);
			janus_refcount_decrease(&videoroom->ref);
			response = json_object();
			json_object_set_new(response, "videoroom", json_string("success"));
			/* Done */
			goto plugin_response;
		}
		participant->kicked = TRUE;
		participant->session->started = FALSE;
		participant->audio_active = FALSE;
		participant->video_active = FALSE;
		participant->data_active = FALSE;
		/* Prepare an event for this */
		json_t *kicked = json_object();
		json_object_set_new(kicked, "videoroom", json_string("event"));
		json_object_set_new(kicked, "room", json_integer(participant->room_id));
		json_object_set_new(kicked, "leaving", json_string("ok"));
		json_object_set_new(kicked, "reason", json_string("kicked"));
		int ret = gateway->push_event(participant->session->handle, &janus_videoroom_plugin, NULL, kicked, NULL);
		JANUS_LOG(LOG_VERB, "  >> %d (%s)\n", ret, janus_get_api_error(ret));
		json_decref(kicked);
		janus_mutex_unlock(&videoroom->mutex);
		/* If this room requires valid private_id values, we can kick subscriptions too */
		if(videoroom->require_pvtid && participant->subscriptions != NULL) {
			/* Iterate on the subscriptions we know this user has */
			janus_mutex_lock(&participant->subscribers_mutex);
			GSList *s = participant->subscriptions;
			while(s) {
				janus_videoroom_subscriber *subscriber = (janus_videoroom_subscriber *)s->data;
				if(subscriber) {
					subscriber->kicked = TRUE;
					subscriber->audio = FALSE;
					subscriber->video = FALSE;
					subscriber->data = FALSE;
					/* FIXME We should also close the PeerConnection, but we risk race conditions if we do it here,
					 * so for now we mark the subscriber as kicked and prevent it from getting any media after this */
				}
				s = s->next;
			}
			janus_mutex_unlock(&participant->subscribers_mutex);
		}
		/* This publisher is leaving, tell everybody */
		janus_videoroom_leave_or_unpublish(participant, TRUE, TRUE);
		/* Also notify event handlers */
		if(notify_events && gateway->events_is_enabled()) {
			json_t *info = json_object();
			json_object_set_new(info, "event", json_string("kicked"));
			json_object_set_new(info, "room", json_integer(room_id));
			json_object_set_new(info, "id", json_integer(user_id));
			gateway->notify_event(&janus_videoroom_plugin, session->handle, info);
		}
		/* Tell the core to tear down the PeerConnection, hangup_media will do the rest */
		if(participant && participant->session)
			gateway->close_pc(participant->session->handle);
		JANUS_LOG(LOG_INFO, "Kicked user %"SCNu64" from room %"SCNu64"\n", user_id, room_id);
		/* Prepare response */
		response = json_object();
		json_object_set_new(response, "videoroom", json_string("success"));
		/* Done */
		janus_mutex_unlock(&videoroom->mutex);
		janus_refcount_decrease(&videoroom->ref);
		goto plugin_response;
	} else if(!strcasecmp(request_text, "listparticipants")) {
		/* List all participants in a room, specifying whether they're publishers or just attendees */	
		JANUS_VALIDATE_JSON_OBJECT(root, room_parameters,
			error_code, error_cause, TRUE,
			JANUS_VIDEOROOM_ERROR_MISSING_ELEMENT, JANUS_VIDEOROOM_ERROR_INVALID_ELEMENT);
		if(error_code != 0)
			goto plugin_response;
		json_t *room = json_object_get(root, "room");
		guint64 room_id = json_integer_value(room);
		janus_mutex_lock(&rooms_mutex);
		janus_videoroom *videoroom = NULL;
		error_code = janus_videoroom_access_room(root, FALSE, FALSE, &videoroom, error_cause, sizeof(error_cause));
		janus_mutex_unlock(&rooms_mutex);
		if(error_code != 0)
			goto plugin_response;
		janus_refcount_increase(&videoroom->ref);
		/* Return a list of all participants (whether they're publishing or not) */
		json_t *list = json_array();
		GHashTableIter iter;
		gpointer value;
		janus_mutex_lock(&videoroom->mutex);
		g_hash_table_iter_init(&iter, videoroom->participants);
		while (!g_atomic_int_get(&videoroom->destroyed) && g_hash_table_iter_next(&iter, NULL, &value)) {
			janus_videoroom_publisher *p = value;
			json_t *pl = json_object();
			json_object_set_new(pl, "id", json_integer(p->user_id));
			if(p->display)
				json_object_set_new(pl, "display", json_string(p->display));
			json_object_set_new(pl, "publisher", (p->sdp && p->session->started) ? json_true() : json_false());
			if((p->sdp && p->session->started)) {
				if(p->audio_level_extmap_id > 0)
					json_object_set_new(pl, "talking", p->talking ? json_true() : json_false());
			}
			json_array_append_new(list, pl);
		}
		janus_mutex_unlock(&videoroom->mutex);
		janus_refcount_decrease(&videoroom->ref);
		response = json_object();
		json_object_set_new(response, "videoroom", json_string("participants"));
		json_object_set_new(response, "room", json_integer(room_id));
		json_object_set_new(response, "participants", list);
		goto plugin_response;
	} else if(!strcasecmp(request_text, "listforwarders")) {
		/* List all forwarders in a room */	
		JANUS_VALIDATE_JSON_OBJECT(root, room_parameters,
			error_code, error_cause, TRUE,
			JANUS_VIDEOROOM_ERROR_MISSING_ELEMENT, JANUS_VIDEOROOM_ERROR_INVALID_ELEMENT);
		if(error_code != 0)
			goto plugin_response;
		json_t *room = json_object_get(root, "room");
		guint64 room_id = json_integer_value(room);
		janus_mutex_lock(&rooms_mutex);
		janus_videoroom *videoroom = g_hash_table_lookup(rooms, &room_id);
		if(videoroom == NULL) {
			JANUS_LOG(LOG_ERR, "No such room (%"SCNu64")\n", room_id);
			error_code = JANUS_VIDEOROOM_ERROR_NO_SUCH_ROOM;
			g_snprintf(error_cause, 512, "No such room (%"SCNu64")", room_id);
			janus_mutex_unlock(&rooms_mutex);
			goto plugin_response;
		}
		if(g_atomic_int_get(&videoroom->destroyed)) {
			JANUS_LOG(LOG_ERR, "No such room (%"SCNu64")\n", room_id);
			error_code = JANUS_VIDEOROOM_ERROR_NO_SUCH_ROOM;
			g_snprintf(error_cause, 512, "No such room (%"SCNu64")", room_id);
			janus_mutex_unlock(&rooms_mutex);
			goto plugin_response;
		}
		/* A secret may be required for this action */
		JANUS_CHECK_SECRET(videoroom->room_secret, root, "secret", error_code, error_cause,
			JANUS_VIDEOROOM_ERROR_MISSING_ELEMENT, JANUS_VIDEOROOM_ERROR_INVALID_ELEMENT, JANUS_VIDEOROOM_ERROR_UNAUTHORIZED);
		if(error_code != 0) {
			janus_mutex_unlock(&rooms_mutex);
			goto plugin_response;
		}
		/* Return a list of all forwarders */
		json_t *list = json_array();
		GHashTableIter iter;
		gpointer value;
		janus_mutex_lock(&videoroom->mutex);
		g_hash_table_iter_init(&iter, videoroom->participants);
		while (!g_atomic_int_get(&videoroom->destroyed) && g_hash_table_iter_next(&iter, NULL, &value)) {
			janus_videoroom_publisher *p = value;
			janus_mutex_lock(&p->rtp_forwarders_mutex);
			if(g_hash_table_size(p->rtp_forwarders) == 0) {
				janus_mutex_unlock(&p->rtp_forwarders_mutex);
				continue;
			}
			json_t *pl = json_object();
			json_object_set_new(pl, "publisher_id", json_integer(p->user_id));
			if(p->display)
				json_object_set_new(pl, "display", json_string(p->display));
			json_t *flist = json_array();
			GHashTableIter iter_f;
			gpointer key_f, value_f;			
			g_hash_table_iter_init(&iter_f, p->rtp_forwarders);
			while(g_hash_table_iter_next(&iter_f, &key_f, &value_f)) {				
				json_t *fl = json_object();
				guint32 rpk = GPOINTER_TO_UINT(key_f);
				janus_videoroom_rtp_forwarder *rpv = value_f;
				json_object_set_new(fl, "ip", json_string(inet_ntoa(rpv->serv_addr.sin_addr)));
				if(rpv->is_data) {
					json_object_set_new(fl, "data_stream_id", json_integer(rpk));
					json_object_set_new(fl, "port", json_integer(ntohs(rpv->serv_addr.sin_port)));
				} else if(rpv->is_video) {
					json_object_set_new(fl, "video_stream_id", json_integer(rpk));
					json_object_set_new(fl, "port", json_integer(ntohs(rpv->serv_addr.sin_port)));
					if(rpv->payload_type)
						json_object_set_new(fl, "pt", json_integer(rpv->payload_type));
					if(rpv->ssrc)
						json_object_set_new(fl, "ssrc", json_integer(rpv->ssrc));
					if(rpv->substream)
						json_object_set_new(fl, "substream", json_integer(rpv->substream));
				} else {
					json_object_set_new(fl, "audio_stream_id", json_integer(rpk));
					json_object_set_new(fl, "port", json_integer(ntohs(rpv->serv_addr.sin_port)));
					if(rpv->payload_type)
						json_object_set_new(fl, "pt", json_integer(rpv->payload_type));
					if(rpv->ssrc)
						json_object_set_new(fl, "ssrc", json_integer(rpv->ssrc));
				}
				if(rpv->is_srtp)
					json_object_set_new(fl, "srtp", json_true());
				json_array_append_new(flist, fl);
			}		
			janus_mutex_unlock(&p->rtp_forwarders_mutex);
			json_object_set_new(pl, "rtp_forwarder", flist);
			json_array_append_new(list, pl);
		}
		janus_mutex_unlock(&videoroom->mutex);
		janus_mutex_unlock(&rooms_mutex);
		response = json_object();
		json_object_set_new(response, "videoroom", json_string("forwarders"));
		json_object_set_new(response, "room", json_integer(room_id));
		json_object_set_new(response, "rtp_forwarders", list);
		goto plugin_response;
	} else if(!strcasecmp(request_text, "join") || !strcasecmp(request_text, "joinandconfigure")
			|| !strcasecmp(request_text, "configure") || !strcasecmp(request_text, "publish") || !strcasecmp(request_text, "unpublish")
			|| !strcasecmp(request_text, "start") || !strcasecmp(request_text, "pause") || !strcasecmp(request_text, "switch")
			|| !strcasecmp(request_text, "leave")) {
		/* These messages are handled asynchronously */

		janus_videoroom_message *msg = g_malloc(sizeof(janus_videoroom_message));
		msg->handle = handle;
		msg->transaction = transaction;
		msg->message = root;
		msg->jsep = jsep;
		g_async_queue_push(messages, msg);

		return janus_plugin_result_new(JANUS_PLUGIN_OK_WAIT, NULL, NULL);
	} else {
		JANUS_LOG(LOG_VERB, "Unknown request '%s'\n", request_text);
		error_code = JANUS_VIDEOROOM_ERROR_INVALID_REQUEST;
		g_snprintf(error_cause, 512, "Unknown request '%s'", request_text);
	}

plugin_response:
		{
			if(error_code == 0 && !response) {
				error_code = JANUS_VIDEOROOM_ERROR_UNKNOWN_ERROR;
				g_snprintf(error_cause, 512, "Invalid response");
			}
			if(error_code != 0) {
				/* Prepare JSON error event */
				json_t *event = json_object();
				json_object_set_new(event, "videoroom", json_string("event"));
				json_object_set_new(event, "error_code", json_integer(error_code));
				json_object_set_new(event, "error", json_string(error_cause));
				response = event;
			}
			if(root != NULL)
				json_decref(root);
			if(jsep != NULL)
				json_decref(jsep);
			g_free(transaction);

			if(session != NULL)
				janus_refcount_decrease(&session->ref);
			return janus_plugin_result_new(JANUS_PLUGIN_OK, NULL, response);
		}

}

void janus_videoroom_setup_media(janus_plugin_session *handle) {
	JANUS_LOG(LOG_INFO, "[%s-%p] WebRTC media is now available\n", JANUS_VIDEOROOM_PACKAGE, handle);
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	janus_mutex_lock(&sessions_mutex);
	janus_videoroom_session *session = janus_videoroom_lookup_session(handle);
	if(!session) {
		janus_mutex_unlock(&sessions_mutex);
		JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
		return;
	}
	if(g_atomic_int_get(&session->destroyed)) {
		janus_mutex_unlock(&sessions_mutex);
		return;
	}
	g_atomic_int_set(&session->hangingup, 0);

	/* Media relaying can start now */
	session->started = TRUE;
	if(session->participant) {
		/* If this is a publisher, notify all subscribers about the fact they can
		 * now subscribe; if this is a subscriber, instead, ask the publisher a FIR */
		if(session->participant_type == janus_videoroom_p_type_publisher) {
			janus_videoroom_publisher *participant = janus_videoroom_session_get_publisher(session);
			/* Notify all other participants that there's a new boy in town */
			json_t *list = json_array();
			json_t *pl = json_object();
			json_object_set_new(pl, "id", json_integer(participant->user_id));
			if(participant->display)
				json_object_set_new(pl, "display", json_string(participant->display));
			if(participant->audio)
				json_object_set_new(pl, "audio_codec", json_string(janus_videoroom_audiocodec_name(participant->acodec)));
			if(participant->video)
				json_object_set_new(pl, "video_codec", json_string(janus_videoroom_videocodec_name(participant->vcodec)));
			json_array_append_new(list, pl);
			json_t *pub = json_object();
			json_object_set_new(pub, "videoroom", json_string("event"));
			json_object_set_new(pub, "room", json_integer(participant->room_id));
			json_object_set_new(pub, "publishers", list);
			GHashTableIter iter;
			gpointer value;
			janus_videoroom *videoroom = participant->room;
			janus_mutex_lock(&videoroom->mutex);
			g_hash_table_iter_init(&iter, videoroom->participants);
			while (!g_atomic_int_get(&videoroom->destroyed) && g_hash_table_iter_next(&iter, NULL, &value)) {
				janus_videoroom_publisher *p = value;
				if(p == participant) {
					continue;	/* Skip the new publisher itself */
				}
				JANUS_LOG(LOG_VERB, "Notifying participant %"SCNu64" (%s)\n", p->user_id, p->display ? p->display : "??");
				int ret = gateway->push_event(p->session->handle, &janus_videoroom_plugin, NULL, pub, NULL);
				JANUS_LOG(LOG_VERB, "  >> %d (%s)\n", ret, janus_get_api_error(ret));
			}
			json_decref(pub);
			/* Also notify event handlers */
			if(notify_events && gateway->events_is_enabled()) {
				json_t *info = json_object();
				json_object_set_new(info, "event", json_string("published"));
				json_object_set_new(info, "room", json_integer(participant->room_id));
				json_object_set_new(info, "id", json_integer(participant->user_id));
				gateway->notify_event(&janus_videoroom_plugin, session->handle, info);
			}
			janus_mutex_unlock(&videoroom->mutex);
			janus_refcount_decrease(&participant->ref);
		} else if(session->participant_type == janus_videoroom_p_type_subscriber) {
			janus_videoroom_subscriber *s = (janus_videoroom_subscriber *)session->participant;
			if(s && s->feed) {
				janus_videoroom_publisher *p = s->feed;
				if(p && p->session) {
					/* Send a FIR */
					char buf[20];
					janus_rtcp_fir((char *)&buf, 20, &p->fir_seq);
					JANUS_LOG(LOG_VERB, "New subscriber available, sending FIR to %"SCNu64" (%s)\n", p->user_id, p->display ? p->display : "??");
					gateway->relay_rtcp(p->session->handle, 1, buf, 20);
					/* Send a PLI too, just in case... */
					janus_rtcp_pli((char *)&buf, 12);
					JANUS_LOG(LOG_VERB, "New subscriber available, sending PLI to %"SCNu64" (%s)\n", p->user_id, p->display ? p->display : "??");
					gateway->relay_rtcp(p->session->handle, 1, buf, 12);
					/* Also notify event handlers */
					if(notify_events && gateway->events_is_enabled()) {
						json_t *info = json_object();
						json_object_set_new(info, "event", json_string("subscribed"));
						json_object_set_new(info, "room", json_integer(p->room_id));
						json_object_set_new(info, "feed", json_integer(p->user_id));
						gateway->notify_event(&janus_videoroom_plugin, session->handle, info);
					}
				}
			}
		}
	}
	janus_mutex_unlock(&sessions_mutex);
}

void janus_videoroom_incoming_rtp(janus_plugin_session *handle, int video, char *buf, int len) {
	if(handle == NULL || g_atomic_int_get(&handle->stopped) || g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized) || !gateway)
		return;
	janus_videoroom_session *session = (janus_videoroom_session *)handle->plugin_handle;
	if(!session || g_atomic_int_get(&session->destroyed) || session->participant_type != janus_videoroom_p_type_publisher)
		return;
	janus_videoroom_publisher *participant = janus_videoroom_session_get_publisher_nodebug(session);
	if(participant == NULL)
		return;
	if(g_atomic_int_get(&participant->destroyed) || participant->kicked || participant->room == NULL) {
		janus_videoroom_publisher_dereference_nodebug(participant);
		return;
	}
	janus_videoroom *videoroom = participant->room;

	/* In case this is an audio packet and we're doing talk detection, check the audio level extension */
	if(!video && videoroom->audiolevel_event && participant->audio_active) {
		int level = 0;
		if(janus_rtp_header_extension_parse_audio_level(buf, len, participant->audio_level_extmap_id, &level) == 0) {
			participant->audio_dBov_sum += level;
			participant->audio_active_packets++;
			participant->audio_dBov_level = level;
			if(participant->audio_active_packets > 0 && participant->audio_active_packets == videoroom->audio_active_packets) {
				gboolean notify_talk_event = FALSE;
				if((float)participant->audio_dBov_sum/(float)participant->audio_active_packets < videoroom->audio_level_average) {
					/* Participant talking, should we notify all participants? */
					if(!participant->talking)
						notify_talk_event = TRUE;
					participant->talking = TRUE;
				} else {
					/* Participant not talking anymore, should we notify all participants? */
					if(participant->talking)
						notify_talk_event = TRUE;
					participant->talking = FALSE;
				}
				participant->audio_active_packets = 0;
				participant->audio_dBov_sum = 0;
				/* Only notify in case of state changes */
				if(notify_talk_event) {
					janus_mutex_lock(&videoroom->mutex);
					json_t *event = json_object();
					json_object_set_new(event, "videoroom", json_string(participant->talking ? "talking" : "stopped-talking"));
					json_object_set_new(event, "room", json_integer(videoroom->room_id));
					json_object_set_new(event, "id", json_integer(participant->user_id));
					janus_videoroom_notify_participants(participant, event);
					json_decref(event);
					janus_mutex_unlock(&videoroom->mutex);
					/* Also notify event handlers */
					if(notify_events && gateway->events_is_enabled()) {
						json_t *info = json_object();
						json_object_set_new(info, "videoroom", json_string(participant->talking ? "talking" : "stopped-talking"));
						json_object_set_new(info, "room", json_integer(videoroom->room_id));
						json_object_set_new(info, "id", json_integer(participant->user_id));
						gateway->notify_event(&janus_videoroom_plugin, session->handle, info);
					}
				}
			}
		}
	}

	if((!video && participant->audio_active) || (video && participant->video_active)) {
		janus_rtp_header *rtp = (janus_rtp_header *)buf;
		int sc = -1;
		/* Check if we're simulcasting, and if so, keep track of the "layer" */
		if(video && participant->ssrc[0] != 0) {
			uint32_t ssrc = ntohl(rtp->ssrc);
			if(ssrc == participant->ssrc[0])
				sc = 0;
			else if(ssrc == participant->ssrc[1])
				sc = 1;
			else if(ssrc == participant->ssrc[2])
				sc = 2;
		}
		/* Forward RTP to the appropriate port for the rtp_forwarders associated with this publisher, if there are any */
		janus_mutex_lock(&participant->rtp_forwarders_mutex);
		if(participant->srtp_contexts && g_hash_table_size(participant->srtp_contexts) > 0) {
			GHashTableIter iter;
			gpointer value;
			g_hash_table_iter_init(&iter, participant->srtp_contexts);
			while(g_hash_table_iter_next(&iter, NULL, &value)) {
				janus_videoroom_srtp_context *srtp_ctx = (janus_videoroom_srtp_context *)value;
				srtp_ctx->slen = 0;
			}
		}
		GHashTableIter iter;
		gpointer value;
		g_hash_table_iter_init(&iter, participant->rtp_forwarders);
		while(participant->udp_sock > 0 && g_hash_table_iter_next(&iter, NULL, &value)) {
			janus_videoroom_rtp_forwarder *rtp_forward = (janus_videoroom_rtp_forwarder *)value;
			/* Check if payload type and/or SSRC need to be overwritten for this forwarder */
			int pt = rtp->type;
			uint32_t ssrc = ntohl(rtp->ssrc);
			if(rtp_forward->payload_type > 0)
				rtp->type = rtp_forward->payload_type;
			if(rtp_forward->ssrc > 0)
				rtp->ssrc = htonl(rtp_forward->ssrc);
			if((video && rtp_forward->is_video && (sc == -1 || rtp_forward->substream == sc)) ||
					(!video && !rtp_forward->is_video && !rtp_forward->is_data)) {
				/* Check if this is an RTP or SRTP forwarder */
				if(!rtp_forward->is_srtp) {
					/* Plain RTP */
					if(sendto(participant->udp_sock, buf, len, 0, (struct sockaddr*)&rtp_forward->serv_addr, sizeof(rtp_forward->serv_addr)) < 0) {
						JANUS_LOG(LOG_HUGE, "Error forwarding RTP %s packet for %s... %s (len=%d)...\n",
							(video ? "video" : "audio"), participant->display, strerror(errno), len);
					}
				} else {
					/* SRTP: check if we already encrypted the packet before */
					if(rtp_forward->srtp_ctx->slen == 0) {
						memcpy(&rtp_forward->srtp_ctx->sbuf, buf, len);
						int protected = len;
						int res = srtp_protect(rtp_forward->srtp_ctx->ctx, &rtp_forward->srtp_ctx->sbuf, &protected);
						if(res != srtp_err_status_ok) {
							janus_rtp_header *header = (janus_rtp_header *)&rtp_forward->srtp_ctx->sbuf;
							guint32 timestamp = ntohl(header->timestamp);
							guint16 seq = ntohs(header->seq_number);
							JANUS_LOG(LOG_ERR, "Error encrypting %s packet for %s... %s (len=%d-->%d, ts=%"SCNu32", seq=%"SCNu16")...\n",
								(video ? "Video" : "Audio"), participant->display, janus_srtp_error_str(res), len, protected, timestamp, seq);
						} else {
							rtp_forward->srtp_ctx->slen = protected;
						}
					}
					if(rtp_forward->srtp_ctx->slen > 0 && sendto(participant->udp_sock, rtp_forward->srtp_ctx->sbuf, rtp_forward->srtp_ctx->slen, 0, (struct sockaddr*)&rtp_forward->serv_addr, sizeof(rtp_forward->serv_addr)) < 0) {
						JANUS_LOG(LOG_HUGE, "Error forwarding SRTP %s packet for %s... %s (len=%d)...\n",
							(video ? "video" : "audio"), participant->display, strerror(errno), rtp_forward->srtp_ctx->slen);
					}
				}
			}
			/* Restore original values of payload type and SSRC before going on */
			rtp->type = pt;
			rtp->ssrc = htonl(ssrc);
		}
		janus_mutex_unlock(&participant->rtp_forwarders_mutex);
		/* Set the payload type of the publisher */
		rtp->type = video ? participant->video_pt : participant->audio_pt;
		if(sc < 1) {
			/* Save the frame if we're recording
			 * FIXME: for video, we're currently only recording the base substream, when simulcasting */
			janus_recorder_save_frame(video ? participant->vrc : participant->arc, buf, len);
		}
		/* Done, relay it */
		janus_videoroom_rtp_relay_packet packet;
		packet.data = rtp;
		packet.length = len;
		packet.is_video = video;
		packet.svc = FALSE;
		if(video && videoroom->do_svc) {
			/* We're doing SVC: let's parse this packet to see which layers are there */
			int plen = 0;
			char *payload = janus_rtp_payload(buf, len, &plen);
			if(payload == NULL)
				return;
			uint8_t pbit = 0, dbit = 0, ubit = 0, bbit = 0, ebit = 0;
			int found = 0, spatial_layer = 0, temporal_layer = 0;
			if(janus_vp9_parse_svc(payload, plen, &found, &spatial_layer, &temporal_layer, &pbit, &dbit, &ubit, &bbit, &ebit) == 0) {
				if(found) {
					packet.svc = TRUE;
					packet.spatial_layer = spatial_layer;
					packet.temporal_layer = temporal_layer;
					packet.pbit = pbit;
					packet.dbit = dbit;
					packet.ubit = ubit;
					packet.bbit = bbit;
					packet.ebit = ebit;
				}
			}
		}
		packet.ssrc[0] = (sc != -1 ? participant->ssrc[0] : 0);
		packet.ssrc[1] = (sc != -1 ? participant->ssrc[1] : 0);
		packet.ssrc[2] = (sc != -1 ? participant->ssrc[2] : 0);
		/* Backup the actual timestamp and sequence number set by the publisher, in case switching is involved */
		packet.timestamp = ntohl(packet.data->timestamp);
		packet.seq_number = ntohs(packet.data->seq_number);
		/* Go: some viewers may decide to drop the packet, but that's up to them */
		janus_mutex_lock_nodebug(&participant->subscribers_mutex);
		g_slist_foreach(participant->subscribers, janus_videoroom_relay_rtp_packet, &packet);
		janus_mutex_unlock_nodebug(&participant->subscribers_mutex);

		/* Check if we need to send any REMB, FIR or PLI back to this publisher */
		if(video && participant->video_active) {
			/* Did we send a REMB already, or is it time to send one? */
			gboolean send_remb = FALSE;
			if(participant->remb_latest == 0 && participant->remb_startup > 0) {
				/* Still in the starting phase, send the ramp-up REMB feedback */
				send_remb = TRUE;
			} else if(participant->remb_latest > 0 && janus_get_monotonic_time()-participant->remb_latest >= 5*G_USEC_PER_SEC) {
				/* 5 seconds have passed since the last REMB, send a new one */
				send_remb = TRUE;
			}
			
			if(send_remb && participant->bitrate) {
				/* We send a few incremental REMB messages at startup */
				uint32_t bitrate = participant->bitrate;
				if(participant->remb_startup > 0) {
					bitrate = bitrate/participant->remb_startup;
					participant->remb_startup--;
				}
				JANUS_LOG(LOG_VERB, "Sending REMB (%s, %"SCNu32")\n", participant->display, bitrate);
				char rtcpbuf[24];
				janus_rtcp_remb((char *)(&rtcpbuf), 24, bitrate);
				gateway->relay_rtcp(handle, video, rtcpbuf, 24);
				if(participant->remb_startup == 0)
					participant->remb_latest = janus_get_monotonic_time();
			}
			/* Generate FIR/PLI too, if needed */
			if(video && participant->video_active && (videoroom->fir_freq > 0)) {
				/* We generate RTCP every tot seconds/frames */
				gint64 now = janus_get_monotonic_time();
				/* First check if this is a keyframe, though: if so, we reset the timer */
				int plen = 0;
				char *payload = janus_rtp_payload(buf, len, &plen);
				if(payload == NULL)
					return;
				if(participant->vcodec == JANUS_VIDEOROOM_VP8) {
					if(janus_vp8_is_keyframe(payload, plen))
						participant->fir_latest = now;
				} else if(participant->vcodec == JANUS_VIDEOROOM_VP9) {
					if(janus_vp9_is_keyframe(payload, plen))
						participant->fir_latest = now;
				} else if(participant->vcodec == JANUS_VIDEOROOM_H264) {
					if(janus_h264_is_keyframe(payload, plen))
						participant->fir_latest = now;
				}
				if((now-participant->fir_latest) >= ((gint64)videoroom->fir_freq*G_USEC_PER_SEC)) {
					/* FIXME We send a FIR every tot seconds */
					participant->fir_latest = now;
					char rtcpbuf[24];
					janus_rtcp_fir((char *)&rtcpbuf, 20, &participant->fir_seq);
					JANUS_LOG(LOG_VERB, "Sending FIR to %"SCNu64" (%s)\n", participant->user_id, participant->display ? participant->display : "??");
					gateway->relay_rtcp(handle, video, rtcpbuf, 20);
					/* Send a PLI too, just in case... */
					janus_rtcp_pli((char *)&rtcpbuf, 12);
					JANUS_LOG(LOG_VERB, "Sending PLI to %"SCNu64" (%s)\n", participant->user_id, participant->display ? participant->display : "??");
					gateway->relay_rtcp(handle, video, rtcpbuf, 12);
				}
			}
		}
	}
	janus_videoroom_publisher_dereference_nodebug(participant);
}

void janus_videoroom_incoming_rtcp(janus_plugin_session *handle, int video, char *buf, int len) {
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	janus_videoroom_session *session = (janus_videoroom_session *)handle->plugin_handle;	
	if(!session) {
		JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
		return;
	}
	if(g_atomic_int_get(&session->destroyed))
		return;
	if(session->participant_type == janus_videoroom_p_type_subscriber) {
		/* A subscriber sent some RTCP, check what it is and if we need to forward it to the publisher */
		janus_videoroom_subscriber *s = (janus_videoroom_subscriber *)session->participant;
		if(s == NULL || g_atomic_int_get(&s->destroyed))
			return;
		if(!s->video)
			return;	/* The only feedback we handle is video related anyway... */
		if(janus_rtcp_has_fir(buf, len)) {
			/* We got a FIR, forward it to the publisher */
			if(s->feed) {
				janus_videoroom_publisher *p = s->feed;
				if(p && p->session) {
					char rtcpbuf[20];
					janus_rtcp_fir((char *)&rtcpbuf, 20, &p->fir_seq);
					JANUS_LOG(LOG_VERB, "Got a FIR from a subscriber, forwarding it to %"SCNu64" (%s)\n", p->user_id, p->display ? p->display : "??");
					gateway->relay_rtcp(p->session->handle, 1, rtcpbuf, 20);
					/* Update the time of when we last sent a keyframe request */
					p->fir_latest = janus_get_monotonic_time();
				}
			}
		}
		if(janus_rtcp_has_pli(buf, len)) {
			/* We got a PLI, forward it to the publisher */
			if(s->feed) {
				janus_videoroom_publisher *p = s->feed;
				if(p && p->session) {
					char rtcpbuf[12];
					janus_rtcp_pli((char *)&rtcpbuf, 12);
					JANUS_LOG(LOG_VERB, "Got a PLI from a subscriber, forwarding it to %"SCNu64" (%s)\n", p->user_id, p->display ? p->display : "??");
					gateway->relay_rtcp(p->session->handle, 1, rtcpbuf, 12);
					/* Update the time of when we last sent a keyframe request */
					p->fir_latest = janus_get_monotonic_time();
				}
			}
		}
		uint32_t bitrate = janus_rtcp_get_remb(buf, len);
		if(bitrate > 0) {
			/* FIXME We got a REMB from this subscriber, should we do something about it? */
		}
	}
}

void janus_videoroom_incoming_data(janus_plugin_session *handle, char *buf, int len) {
	if(handle == NULL || g_atomic_int_get(&handle->stopped) || g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized) || !gateway)
		return;
	if(buf == NULL || len <= 0)
		return;
	janus_videoroom_session *session = (janus_videoroom_session *)handle->plugin_handle;
	if(!session || g_atomic_int_get(&session->destroyed) || session->participant_type != janus_videoroom_p_type_publisher)
		return;
	janus_videoroom_publisher *participant = janus_videoroom_session_get_publisher_nodebug(session);
	if(participant == NULL)
		return;
	if(g_atomic_int_get(&participant->destroyed) || !participant->data_active || participant->kicked) {
		janus_videoroom_publisher_dereference_nodebug(participant);
		return;
	}
	/* Any forwarder involved? */
	janus_mutex_lock(&participant->rtp_forwarders_mutex);
	/* Forward RTP to the appropriate port for the rtp_forwarders associated with this publisher, if there are any */
	GHashTableIter iter;
	gpointer value;
	g_hash_table_iter_init(&iter, participant->rtp_forwarders);
	while(participant->udp_sock > 0 && g_hash_table_iter_next(&iter, NULL, &value)) {
		janus_videoroom_rtp_forwarder* rtp_forward = (janus_videoroom_rtp_forwarder*)value;
		if(rtp_forward->is_data) {
			if(sendto(participant->udp_sock, buf, len, 0, (struct sockaddr*)&rtp_forward->serv_addr, sizeof(rtp_forward->serv_addr)) < 0) {
				JANUS_LOG(LOG_HUGE, "Error forwarding data packet for %s... %s (len=%d)...\n",
					participant->display, strerror(errno), len);
			}
		}
	}
	janus_mutex_unlock(&participant->rtp_forwarders_mutex);
	/* Get a string out of the data */
	char *text = g_malloc(len+1);
	memcpy(text, buf, len);
	*(text+len) = '\0';
	JANUS_LOG(LOG_VERB, "Got a DataChannel message (%zu bytes) to forward: %s\n", strlen(text), text);
	/* Save the message if we're recording */
	janus_recorder_save_frame(participant->drc, text, strlen(text));
	/* Relay to all subscribers */
	janus_mutex_lock_nodebug(&participant->subscribers_mutex);
	g_slist_foreach(participant->subscribers, janus_videoroom_relay_data_packet, text);
	janus_mutex_unlock_nodebug(&participant->subscribers_mutex);
	g_free(text);
	janus_videoroom_publisher_dereference_nodebug(participant);
}

void janus_videoroom_slow_link(janus_plugin_session *handle, int uplink, int video) {
	/* The core is informing us that our peer got too many NACKs, are we pushing media too hard? */
	if(handle == NULL || g_atomic_int_get(&handle->stopped) || g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized) || !gateway)
		return;
	janus_mutex_lock(&sessions_mutex);
	janus_videoroom_session *session = janus_videoroom_lookup_session(handle);
	if(!session || g_atomic_int_get(&session->destroyed) || !session->participant) {
		janus_mutex_unlock(&sessions_mutex);
		return;
	}
	janus_refcount_increase(&session->ref);
	janus_mutex_unlock(&sessions_mutex);
	/* Check if it's an uplink (publisher) or downlink (viewer) issue */
	if(session->participant_type == janus_videoroom_p_type_publisher) {
		if(!uplink) {
			janus_videoroom_publisher *publisher = janus_videoroom_session_get_publisher(session);
			if(publisher == NULL || g_atomic_int_get(&publisher->destroyed)) {
				janus_refcount_decrease(&session->ref);
				janus_refcount_decrease(&publisher->ref);
				return;
			}
			/* Send an event on the handle to notify the application: it's
			 * up to the application to then choose a policy and enforce it */
			json_t *event = json_object();
			json_object_set_new(event, "videoroom", json_string("slow_link"));
			/* Also add info on what the current bitrate cap is */
			uint32_t bitrate = publisher->bitrate;
			json_object_set_new(event, "current-bitrate", json_integer(bitrate));
			gateway->push_event(session->handle, &janus_videoroom_plugin, NULL, event, NULL);
			json_decref(event);
			janus_refcount_decrease(&publisher->ref);
		} else {
			JANUS_LOG(LOG_WARN, "Got a slow uplink on a VideoRoom publisher? Weird, because it doesn't receive media...\n");
		}
	} else if(session->participant_type == janus_videoroom_p_type_subscriber) {
		if(uplink) {
			janus_videoroom_subscriber *viewer = (janus_videoroom_subscriber *)session->participant;
			if(viewer == NULL || g_atomic_int_get(&viewer->destroyed)) {
				janus_refcount_decrease(&session->ref);
				return;
			}
			/* Send an event on the handle to notify the application: it's
			 * up to the application to then choose a policy and enforce it */
			json_t *event = json_object();
			json_object_set_new(event, "videoroom", json_string("slow_link"));
			gateway->push_event(session->handle, &janus_videoroom_plugin, NULL, event, NULL);
			json_decref(event);
		} else {
			JANUS_LOG(LOG_WARN, "Got a slow downlink on a VideoRoom viewer? Weird, because it doesn't send media...\n");
		}
	}
	janus_refcount_decrease(&session->ref);
}

static void janus_videoroom_recorder_create(janus_videoroom_publisher *participant, gboolean audio, gboolean video, gboolean data) {
	char filename[255];
	gint64 now = janus_get_real_time();
	if(audio) {
		memset(filename, 0, 255);
		if(participant->recording_base) {
			/* Use the filename and path we have been provided */
			g_snprintf(filename, 255, "%s-audio", participant->recording_base);
			participant->arc = janus_recorder_create(participant->room->rec_dir,
				janus_videoroom_audiocodec_name(participant->acodec), filename);
			if(participant->arc == NULL) {
				JANUS_LOG(LOG_ERR, "Couldn't open an audio recording file for this publisher!\n");
			}
		} else {
			/* Build a filename */
			g_snprintf(filename, 255, "videoroom-%"SCNu64"-user-%"SCNu64"-%"SCNi64"-audio",
				participant->room_id, participant->user_id, now);
			participant->arc = janus_recorder_create(participant->room->rec_dir,
				janus_videoroom_audiocodec_name(participant->acodec), filename);
			if(participant->arc == NULL) {
				JANUS_LOG(LOG_ERR, "Couldn't open an audio recording file for this publisher!\n");
			}
		}
	}
	if(video) {
		memset(filename, 0, 255);
		if(participant->recording_base) {
			/* Use the filename and path we have been provided */
			g_snprintf(filename, 255, "%s-video", participant->recording_base);
			participant->vrc = janus_recorder_create(participant->room->rec_dir,
				janus_videoroom_videocodec_name(participant->vcodec), filename);
			if(participant->vrc == NULL) {
				JANUS_LOG(LOG_ERR, "Couldn't open an video recording file for this publisher!\n");
			}
		} else {
			/* Build a filename */
			g_snprintf(filename, 255, "videoroom-%"SCNu64"-user-%"SCNu64"-%"SCNi64"-video",
				participant->room_id, participant->user_id, now);
			participant->vrc = janus_recorder_create(participant->room->rec_dir,
				janus_videoroom_videocodec_name(participant->vcodec), filename);
			if(participant->vrc == NULL) {
				JANUS_LOG(LOG_ERR, "Couldn't open an video recording file for this publisher!\n");
			}
		}
	}
	if(data) {
		memset(filename, 0, 255);
		if(participant->recording_base) {
			/* Use the filename and path we have been provided */
			g_snprintf(filename, 255, "%s-data", participant->recording_base);
			participant->drc = janus_recorder_create(participant->room->rec_dir,
				"text", filename);
			if(participant->drc == NULL) {
				JANUS_LOG(LOG_ERR, "Couldn't open an data recording file for this publisher!\n");
			}
		} else {
			/* Build a filename */
			g_snprintf(filename, 255, "videoroom-%"SCNu64"-user-%"SCNu64"-%"SCNi64"-data",
				participant->room_id, participant->user_id, now);
			participant->drc = janus_recorder_create(participant->room->rec_dir,
				"text", filename);
			if(participant->drc == NULL) {
				JANUS_LOG(LOG_ERR, "Couldn't open an data recording file for this publisher!\n");
			}
		}
	}
}

static void janus_videoroom_recorder_close(janus_videoroom_publisher *participant) {
	if(participant->arc) {
		janus_recorder_close(participant->arc);
		JANUS_LOG(LOG_INFO, "Closed audio recording %s\n", participant->arc->filename ? participant->arc->filename : "??");
		janus_recorder_destroy(participant->arc);
	}
	participant->arc = NULL;
	if(participant->vrc) {
		janus_recorder_close(participant->vrc);
		JANUS_LOG(LOG_INFO, "Closed video recording %s\n", participant->vrc->filename ? participant->vrc->filename : "??");
		janus_recorder_destroy(participant->vrc);
	}
	participant->vrc = NULL;
	if(participant->drc) {
		janus_recorder_close(participant->drc);
		JANUS_LOG(LOG_INFO, "Closed data recording %s\n", participant->drc->filename ? participant->drc->filename : "??");
		janus_recorder_destroy(participant->drc);
	}
	participant->drc = NULL;
}

void janus_videoroom_hangup_media(janus_plugin_session *handle) {
	JANUS_LOG(LOG_INFO, "[%s-%p] No WebRTC media anymore; %p %p\n", JANUS_VIDEOROOM_PACKAGE, handle, handle->gateway_handle, handle->plugin_handle);
	janus_mutex_lock(&sessions_mutex);
	janus_videoroom_hangup_media_internal(handle);
	janus_mutex_unlock(&sessions_mutex);
}

static void janus_videoroom_hangup_media_internal(janus_plugin_session *handle) {
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	janus_videoroom_session *session = janus_videoroom_lookup_session(handle);
	if(!session) {
		JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
		return;
	}
	session->started = FALSE;
	if(g_atomic_int_get(&session->destroyed)) {
		return;
	}
	if(g_atomic_int_add(&session->hangingup, 1)) {
		return;
	}
	/* Send an event to the browser and tell the PeerConnection is over */
	if(session->participant_type == janus_videoroom_p_type_publisher) {
		/* This publisher just 'unpublished' */
		janus_videoroom_publisher *participant = janus_videoroom_session_get_publisher(session);
		/* Get rid of the recorders, if available */
		janus_mutex_lock(&participant->rec_mutex);
		g_free(participant->recording_base);
		participant->recording_base = NULL;
		janus_videoroom_recorder_close(participant);
		janus_mutex_unlock(&participant->rec_mutex);
		/* Use subscribers_mutex to protect fields used in janus_videoroom_incoming_rtp */
		janus_mutex_lock(&participant->subscribers_mutex);
		g_free(participant->sdp);
		participant->sdp = NULL;
		participant->firefox = FALSE;
		participant->audio_active = FALSE;
		participant->video_active = FALSE;
		participant->data_active = FALSE;
		participant->audio_active_packets = 0;
		participant->audio_dBov_sum = 0;
		participant->audio_dBov_level = 0;
		participant->talking = FALSE;
		participant->remb_startup = 4;
		participant->remb_latest = 0;
		participant->fir_latest = 0;
		participant->fir_seq = 0;
		while(participant->subscribers) {
			janus_videoroom_subscriber *s = (janus_videoroom_subscriber *)participant->subscribers->data;
			if(s) {
				participant->subscribers = g_slist_remove(participant->subscribers, s);
				janus_refcount_decrease(&participant->session->ref);
				if(s->feed)
					g_clear_pointer(&s->feed, janus_videoroom_publisher_dereference);
				if(s->room)
					g_clear_pointer(&s->room, janus_videoroom_room_dereference);
				janus_refcount_decrease(&s->ref);
			}
		}
		janus_mutex_unlock(&participant->subscribers_mutex);
		janus_videoroom_leave_or_unpublish(participant, FALSE, FALSE);
		/* Also notify event handlers */
		if(participant->room && gateway->events_is_enabled()) {
			json_t *info = json_object();
			json_object_set_new(info, "event", json_string("unpublished"));
			json_object_set_new(info, "room", json_integer(participant->room_id));
			json_object_set_new(info, "id", json_integer(participant->user_id));
			gateway->notify_event(&janus_videoroom_plugin, handle, info);
		}
		janus_refcount_decrease(&participant->ref);
	} else if(session->participant_type == janus_videoroom_p_type_subscriber) {
		/* Get rid of subscriber */
		janus_videoroom_subscriber *subscriber = (janus_videoroom_subscriber *)session->participant;
		if(subscriber) {
			subscriber->paused = TRUE;
			janus_videoroom_publisher *publisher = subscriber->feed;
			if(publisher != NULL) {
				janus_mutex_lock(&publisher->subscribers_mutex);
				publisher->subscribers = g_slist_remove(publisher->subscribers, subscriber);
				if(subscriber->pvt_id > 0) {
					janus_videoroom_publisher *owner = g_hash_table_lookup(publisher->room->private_ids, GUINT_TO_POINTER(subscriber->pvt_id));
					if(owner != NULL) {
						/* Note: we should refcount these subscription-publisher mappings as well */
						owner->subscriptions = g_slist_remove(owner->subscriptions, subscriber);
					}
				}
				/* Also notify event handlers */
				if(notify_events && gateway->events_is_enabled()) {
					json_t *info = json_object();
					json_object_set_new(info, "event", json_string("unsubscribed"));
					json_object_set_new(info, "room", json_integer(publisher->room_id));
					json_object_set_new(info, "feed", json_integer(publisher->user_id));
					gateway->notify_event(&janus_videoroom_plugin, session->handle, info);
				}
				janus_mutex_unlock(&publisher->subscribers_mutex);
				janus_refcount_decrease(&publisher->session->ref);
				if(subscriber->feed)
					g_clear_pointer(&subscriber->feed, janus_videoroom_publisher_dereference);
				if(subscriber->room)
					g_clear_pointer(&subscriber->room, janus_videoroom_room_dereference);
				janus_refcount_decrease(&subscriber->ref);
			}
		}
		/* TODO Should we close the handle as well? */
	}
}

/* Thread to handle incoming messages */
static void *janus_videoroom_handler(void *data) {
	JANUS_LOG(LOG_VERB, "Joining VideoRoom handler thread\n");
	janus_videoroom_message *msg = NULL;
	int error_code = 0;
	char error_cause[512];
	json_t *root = NULL;
	while(g_atomic_int_get(&initialized) && !g_atomic_int_get(&stopping)) {
		msg = g_async_queue_pop(messages);
		if(msg == &exit_message)
			break;
		if(msg->handle == NULL) {
			janus_videoroom_message_free(msg);
			continue;
		}
		janus_videoroom *videoroom = NULL;
		janus_videoroom_publisher *participant = NULL;
		janus_mutex_lock(&sessions_mutex);
		janus_videoroom_session *session = janus_videoroom_lookup_session(msg->handle);
		if(!session) {
			janus_mutex_unlock(&sessions_mutex);
			JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
			janus_videoroom_message_free(msg);
			continue;
		}
		if(g_atomic_int_get(&session->destroyed)) {
			janus_mutex_unlock(&sessions_mutex);
			janus_videoroom_message_free(msg);
			continue;
		}
		janus_mutex_unlock(&sessions_mutex);
		/* Handle request */
		error_code = 0;
		root = NULL;
		if(msg->message == NULL) {
			JANUS_LOG(LOG_ERR, "No message??\n");
			error_code = JANUS_VIDEOROOM_ERROR_NO_MESSAGE;
			g_snprintf(error_cause, 512, "%s", "No message??");
			goto error;
		}
		root = msg->message;
		/* Get the request first */
		JANUS_VALIDATE_JSON_OBJECT(root, request_parameters,
			error_code, error_cause, TRUE,
			JANUS_VIDEOROOM_ERROR_MISSING_ELEMENT, JANUS_VIDEOROOM_ERROR_INVALID_ELEMENT);
		if(error_code != 0)
			goto error;
		json_t *request = json_object_get(root, "request");
		const char *request_text = json_string_value(request);
		json_t *event = NULL;
		gboolean sdp_update = FALSE;
		if(json_object_get(msg->jsep, "update") != NULL)
			sdp_update = json_is_true(json_object_get(msg->jsep, "update"));
		/* 'create' and 'destroy' are handled synchronously: what kind of participant is this session referring to? */
		if(session->participant_type == janus_videoroom_p_type_none) {
			JANUS_LOG(LOG_VERB, "Configuring new participant\n");
			/* Not configured yet, we need to do this now */
			if(strcasecmp(request_text, "join") && strcasecmp(request_text, "joinandconfigure")) {
				JANUS_LOG(LOG_ERR, "Invalid request on unconfigured participant\n");
				error_code = JANUS_VIDEOROOM_ERROR_JOIN_FIRST;
				g_snprintf(error_cause, 512, "Invalid request on unconfigured participant");
				goto error;
			}
			JANUS_VALIDATE_JSON_OBJECT(root, join_parameters,
				error_code, error_cause, TRUE,
				JANUS_VIDEOROOM_ERROR_MISSING_ELEMENT, JANUS_VIDEOROOM_ERROR_INVALID_ELEMENT);
			if(error_code != 0)
				goto error;
			janus_mutex_lock(&rooms_mutex);
			error_code = janus_videoroom_access_room(root, FALSE, TRUE, &videoroom, error_cause, sizeof(error_cause));
			janus_mutex_unlock(&rooms_mutex);
			if(error_code != 0)
				goto error;
			janus_refcount_increase(&videoroom->ref);
			json_t *ptype = json_object_get(root, "ptype");
			const char *ptype_text = json_string_value(ptype);
			if(!strcasecmp(ptype_text, "publisher")) {
				JANUS_LOG(LOG_VERB, "Configuring new publisher\n");
				JANUS_VALIDATE_JSON_OBJECT(root, publisher_parameters,
					error_code, error_cause, TRUE,
					JANUS_VIDEOROOM_ERROR_MISSING_ELEMENT, JANUS_VIDEOROOM_ERROR_INVALID_ELEMENT);
				if(error_code != 0) {
					janus_refcount_decrease(&videoroom->ref);
					goto error;
				}
				/* A token might be required to join */
				if(videoroom->check_allowed) {
					json_t *token = json_object_get(root, "token");
					const char *token_text = token ? json_string_value(token) : NULL;
					if(token_text == NULL || g_hash_table_lookup(videoroom->allowed, token_text) == NULL) {
						janus_refcount_decrease(&videoroom->ref);
						JANUS_LOG(LOG_ERR, "Unauthorized (not in the allowed list)\n");
						error_code = JANUS_VIDEOROOM_ERROR_UNAUTHORIZED;
						g_snprintf(error_cause, 512, "Unauthorized (not in the allowed list)");
						goto error;
					}
				}
				json_t *display = json_object_get(root, "display");
				const char *display_text = display ? json_string_value(display) : NULL;
				guint64 user_id = 0;
				json_t *id = json_object_get(root, "id");
				janus_mutex_lock(&videoroom->mutex);
				if(id) {
					user_id = json_integer_value(id);
					if(g_hash_table_lookup(videoroom->participants, &user_id) != NULL) {
						janus_mutex_unlock(&videoroom->mutex);
						janus_refcount_decrease(&videoroom->ref);
						/* User ID already taken */
						JANUS_LOG(LOG_ERR, "User ID %"SCNu64" already exists\n", user_id);
						error_code = JANUS_VIDEOROOM_ERROR_ID_EXISTS;
						g_snprintf(error_cause, 512, "User ID %"SCNu64" already exists", user_id);
						goto error;
					}
				}
				if(user_id == 0) {
					/* Generate a random ID */
					while(user_id == 0) {
						user_id = janus_random_uint64();
						if(g_hash_table_lookup(videoroom->participants, &user_id) != NULL) {
							/* User ID already taken, try another one */
							user_id = 0;
						}
					}
				}
				JANUS_LOG(LOG_VERB, "  -- Publisher ID: %"SCNu64"\n", user_id);
				/* Process the request */
				json_t *audio = NULL, *video = NULL, *data = NULL,
					*bitrate = NULL, *record = NULL, *recfile = NULL;
				if(!strcasecmp(request_text, "joinandconfigure")) {
					/* Also configure (or publish a new feed) audio/video/bitrate for this new publisher */
					/* join_parameters were validated earlier. */
					audio = json_object_get(root, "audio");
					video = json_object_get(root, "video");
					data = json_object_get(root, "data");
					bitrate = json_object_get(root, "bitrate");
					record = json_object_get(root, "record");
					recfile = json_object_get(root, "filename");
				}
				janus_videoroom_publisher *publisher = g_malloc0(sizeof(janus_videoroom_publisher));
				publisher->session = session;
				publisher->room_id = videoroom->room_id;
				publisher->room = videoroom;
				videoroom = NULL;
				publisher->user_id = user_id;
				publisher->display = display_text ? g_strdup(display_text) : NULL;
				publisher->sdp = NULL;		/* We'll deal with this later */
				publisher->audio = FALSE;	/* We'll deal with this later */
				publisher->video = FALSE;	/* We'll deal with this later */
				publisher->data = FALSE;	/* We'll deal with this later */
				publisher->acodec = JANUS_VIDEOROOM_NOAUDIO;	/* We'll deal with this later */
				publisher->vcodec = JANUS_VIDEOROOM_NOVIDEO;	/* We'll deal with this later */
				publisher->audio_active = FALSE;
				publisher->video_active = FALSE;
				publisher->data_active = FALSE;
				publisher->recording_active = FALSE;
				publisher->recording_base = NULL;
				publisher->arc = NULL;
				publisher->vrc = NULL;
				publisher->drc = NULL;
				janus_mutex_init(&publisher->rec_mutex);
				publisher->firefox = FALSE;
				publisher->bitrate = publisher->room->bitrate;
				publisher->subscribers = NULL;
				publisher->subscriptions = NULL;
				janus_mutex_init(&publisher->subscribers_mutex);
				publisher->audio_pt = -1;	/* We'll deal with this later */
				publisher->video_pt = -1;	/* We'll deal with this later */
				publisher->audio_level_extmap_id = 0;
				publisher->video_orient_extmap_id = 0;
				publisher->playout_delay_extmap_id = 0;
				publisher->transport_wide_cc_extmap_id = 0;
				publisher->remb_startup = 4;
				publisher->remb_latest = 0;
				publisher->fir_latest = 0;
				publisher->fir_seq = 0;
				janus_mutex_init(&publisher->rtp_forwarders_mutex);
				publisher->rtp_forwarders = g_hash_table_new_full(NULL, NULL, NULL, (GDestroyNotify)janus_videoroom_rtp_forwarder_free_helper);
				publisher->srtp_contexts = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, (GDestroyNotify)janus_videoroom_srtp_context_free_helper);
				publisher->udp_sock = -1;
				/* Finally, generate a private ID: this is only needed in case the participant
				 * wants to allow the plugin to know which subscriptions belong to them */
				publisher->pvt_id = 0;
				while(publisher->pvt_id == 0) {
					publisher->pvt_id = janus_random_uint32();
					if(g_hash_table_lookup(publisher->room->private_ids, GUINT_TO_POINTER(publisher->pvt_id)) != NULL) {
						/* Private ID already taken, try another one */
						publisher->pvt_id = 0;
					}
					g_hash_table_insert(publisher->room->private_ids, GUINT_TO_POINTER(publisher->pvt_id), publisher);
				}
				g_atomic_int_set(&publisher->destroyed, 0);
				janus_refcount_init(&publisher->ref, janus_videoroom_publisher_free);
				/* In case we also wanted to configure */
				if(audio) {
					publisher->audio_active = json_is_true(audio);
					JANUS_LOG(LOG_VERB, "Setting audio property: %s (room %"SCNu64", user %"SCNu64")\n", publisher->audio_active ? "true" : "false", publisher->room_id, publisher->user_id);
				}
				if(video) {
					publisher->video_active = json_is_true(video);
					JANUS_LOG(LOG_VERB, "Setting video property: %s (room %"SCNu64", user %"SCNu64")\n", publisher->video_active ? "true" : "false", publisher->room_id, publisher->user_id);
				}
				if(data) {
					publisher->data_active = json_is_true(data);
					JANUS_LOG(LOG_VERB, "Setting data property: %s (room %"SCNu64", user %"SCNu64")\n", publisher->data_active ? "true" : "false", publisher->room_id, publisher->user_id);
				}
				if(bitrate) {
					publisher->bitrate = json_integer_value(bitrate);
					JANUS_LOG(LOG_VERB, "Setting video bitrate: %"SCNu32" (room %"SCNu64", user %"SCNu64")\n", publisher->bitrate, publisher->room_id, publisher->user_id);
				}
				if(record) {
					publisher->recording_active = json_is_true(record);
					JANUS_LOG(LOG_VERB, "Setting record property: %s (room %"SCNu64", user %"SCNu64")\n", publisher->recording_active ? "true" : "false", publisher->room_id, publisher->user_id);
				}
				if(recfile) {
					publisher->recording_base = g_strdup(json_string_value(recfile));
					JANUS_LOG(LOG_VERB, "Setting recording basename: %s (room %"SCNu64", user %"SCNu64")\n", publisher->recording_base, publisher->room_id, publisher->user_id);
				}
				/* Done */
				janus_mutex_lock(&session->mutex);
				session->participant_type = janus_videoroom_p_type_publisher;
				session->participant = publisher;
				janus_mutex_unlock(&session->mutex);
				/* Return a list of all available publishers (those with an SDP available, that is) */
				json_t *list = json_array();
				GHashTableIter iter;
				gpointer value;
				janus_refcount_increase(&publisher->ref);
				g_hash_table_insert(publisher->room->participants, janus_uint64_dup(publisher->user_id), publisher);
				g_hash_table_iter_init(&iter, publisher->room->participants);
				while (!g_atomic_int_get(&publisher->room->destroyed) && g_hash_table_iter_next(&iter, NULL, &value)) {
					janus_videoroom_publisher *p = value;
					if(p == publisher || !p->sdp || !p->session->started) {
						continue;
					}
					json_t *pl = json_object();
					json_object_set_new(pl, "id", json_integer(p->user_id));
					if(p->display)
						json_object_set_new(pl, "display", json_string(p->display));
					if(p->audio)
						json_object_set_new(pl, "audio_codec", json_string(janus_videoroom_audiocodec_name(p->acodec)));
					if(p->video)
						json_object_set_new(pl, "video_codec", json_string(janus_videoroom_videocodec_name(p->vcodec)));
					if(p->audio_level_extmap_id > 0)
						json_object_set_new(pl, "talking", p->talking ? json_true() : json_false());
					json_array_append_new(list, pl);
				}
				event = json_object();
				json_object_set_new(event, "videoroom", json_string("joined"));
				json_object_set_new(event, "room", json_integer(publisher->room->room_id));
				json_object_set_new(event, "description", json_string(publisher->room->room_name));
				json_object_set_new(event, "id", json_integer(user_id));
				json_object_set_new(event, "private_id", json_integer(publisher->pvt_id));
				json_object_set_new(event, "publishers", list);
				/* See if we need to notify about a new participant joined the room (by default, we don't). */
				janus_videoroom_participant_joining(publisher);

				/* Also notify event handlers */
				if(notify_events && gateway->events_is_enabled()) {
					json_t *info = json_object();
					json_object_set_new(info, "event", json_string("joined"));
					json_object_set_new(info, "room", json_integer(publisher->room->room_id));
					json_object_set_new(info, "id", json_integer(user_id));
					json_object_set_new(info, "private_id", json_integer(publisher->pvt_id));
					if(display_text != NULL)
						json_object_set_new(info, "display", json_string(display_text));
					gateway->notify_event(&janus_videoroom_plugin, session->handle, info);
				}
				janus_mutex_unlock(&publisher->room->mutex);
			} else if(!strcasecmp(ptype_text, "subscriber") || !strcasecmp(ptype_text, "listener")) {
				JANUS_LOG(LOG_VERB, "Configuring new subscriber\n");
				gboolean legacy = !strcasecmp(ptype_text, "listener");
				if(legacy) {
					JANUS_LOG(LOG_WARN, "Subscriber is using the legacy 'listener' ptype\n");
				}
				/* This is a new subscriber */
				JANUS_VALIDATE_JSON_OBJECT(root, subscriber_parameters,
					error_code, error_cause, TRUE,
					JANUS_VIDEOROOM_ERROR_MISSING_ELEMENT, JANUS_VIDEOROOM_ERROR_INVALID_ELEMENT);
				if(error_code != 0)
					goto error;
				json_t *feed = json_object_get(root, "feed");
				guint64 feed_id = json_integer_value(feed);
				json_t *pvt = json_object_get(root, "private_id");
				guint64 pvt_id = json_integer_value(pvt);
				json_t *cpc = json_object_get(root, "close_pc");
				gboolean close_pc  = cpc ? json_is_true(cpc) : TRUE;
				json_t *audio = json_object_get(root, "audio");
				json_t *video = json_object_get(root, "video");
				json_t *data = json_object_get(root, "data");
				json_t *offer_audio = json_object_get(root, "offer_audio");
				json_t *offer_video = json_object_get(root, "offer_video");
				json_t *offer_data = json_object_get(root, "offer_data");
				janus_mutex_lock(&videoroom->mutex);
				janus_videoroom_publisher *owner = NULL;
				janus_videoroom_publisher *publisher = g_hash_table_lookup(videoroom->participants, &feed_id);
				if(publisher == NULL || g_atomic_int_get(&publisher->destroyed) || publisher->sdp == NULL) {
					JANUS_LOG(LOG_ERR, "No such feed (%"SCNu64")\n", feed_id);
					error_code = JANUS_VIDEOROOM_ERROR_NO_SUCH_FEED;
					g_snprintf(error_cause, 512, "No such feed (%"SCNu64")", feed_id);
					janus_mutex_unlock(&videoroom->mutex);
					goto error;
				} else {
					/* Increase the refcount before unlocking so that nobody can remove and free the publisher in the meantime. */
					janus_refcount_increase(&publisher->ref);
					janus_refcount_increase(&publisher->session->ref);
					janus_mutex_unlock(&videoroom->mutex);
					/* First of all, let's check if this room requires valid private_id values */
					if(videoroom->require_pvtid) {
						/* It does, let's make sure this subscription complies */
						owner = g_hash_table_lookup(videoroom->private_ids, GUINT_TO_POINTER(pvt_id));
						if(pvt_id == 0 || owner == NULL) {
							JANUS_LOG(LOG_ERR, "Unauthorized (this room requires a valid private_id)\n");
							error_code = JANUS_VIDEOROOM_ERROR_UNAUTHORIZED;
							g_snprintf(error_cause, 512, "Unauthorized (this room requires a valid private_id)");
							goto error;
						}
					}
					janus_videoroom_subscriber *subscriber = g_malloc0(sizeof(janus_videoroom_subscriber));
					subscriber->session = session;
					subscriber->room = videoroom;
					videoroom = NULL;
					subscriber->feed = publisher;
					subscriber->pvt_id = pvt_id;
					subscriber->close_pc = close_pc;
					/* Initialize the subscriber context */
					janus_rtp_switching_context_reset(&subscriber->context);
					subscriber->audio_offered = offer_audio ? json_is_true(offer_audio) : TRUE;	/* True by default */
					subscriber->video_offered = offer_video ? json_is_true(offer_video) : TRUE;	/* True by default */
					subscriber->data_offered = offer_data ? json_is_true(offer_data) : TRUE;	/* True by default */
					if((!publisher->audio || !subscriber->audio_offered) &&
							(!publisher->video || !subscriber->video_offered) &&
							(!publisher->data || !subscriber->data_offered)) {
						g_free(subscriber);
						janus_refcount_decrease(&publisher->session->ref);
						janus_refcount_decrease(&publisher->ref);
						JANUS_LOG(LOG_ERR, "Can't offer an SDP with no audio, video or data\n");
						error_code = JANUS_VIDEOROOM_ERROR_INVALID_SDP;
						g_snprintf(error_cause, 512, "Can't offer an SDP with no audio, video or data");
						goto error;
					}
					subscriber->audio = audio ? json_is_true(audio) : TRUE;	/* True by default */
					if(!publisher->audio || !subscriber->audio_offered)
						subscriber->audio = FALSE;	/* ... unless the publisher isn't sending any audio or we're skipping it */
					subscriber->video = video ? json_is_true(video) : TRUE;	/* True by default */
					if(!publisher->video || !subscriber->video_offered)
						subscriber->video = FALSE;	/* ... unless the publisher isn't sending any video or we're skipping it */
					subscriber->data = data ? json_is_true(data) : TRUE;	/* True by default */
					if(!publisher->data || !subscriber->data_offered)
						subscriber->data = FALSE;	/* ... unless the publisher isn't sending any data or we're skipping it */
					subscriber->paused = TRUE;	/* We need an explicit start from the subscriber */
					g_atomic_int_set(&subscriber->destroyed, 0);
					janus_refcount_init(&subscriber->ref, janus_videoroom_subscriber_free);
					janus_refcount_increase(&subscriber->ref);	/* The publisher references the new subscriber too */
					subscriber->substream = -1;
					subscriber->substream_target = 2;
					subscriber->templayer = -1;
					subscriber->templayer_target = 2;
					subscriber->last_relayed = 0;
					janus_vp8_simulcast_context_reset(&subscriber->simulcast_context);
					if(subscriber->room->do_svc) {
						/* This subscriber belongs to a room where VP9 SVC has been enabled,
						 * let's assume we're interested in all layers for the time being */
						subscriber->spatial_layer = -1;
						subscriber->target_spatial_layer = 1;		/* FIXME Chrome sends 0 and 1 */
						subscriber->temporal_layer = -1;
						subscriber->target_temporal_layer = 2;	/* FIXME Chrome sends 0, 1 and 2 */
					}
					session->participant = subscriber;
					janus_mutex_lock(&publisher->subscribers_mutex);
					publisher->subscribers = g_slist_append(publisher->subscribers, subscriber);
					janus_mutex_unlock(&publisher->subscribers_mutex);
					if(owner != NULL) {
						/* Note: we should refcount these subscription-publisher mappings as well */
						janus_mutex_lock(&owner->subscribers_mutex);
						owner->subscriptions = g_slist_append(owner->subscriptions, subscriber);
						janus_mutex_unlock(&owner->subscribers_mutex);
					}
					event = json_object();
					json_object_set_new(event, "videoroom", json_string("attached"));
					json_object_set_new(event, "room", json_integer(subscriber->room->room_id));
					json_object_set_new(event, "id", json_integer(feed_id));
					if(publisher->display)
						json_object_set_new(event, "display", json_string(publisher->display));
					if(legacy)
						json_object_set_new(event, "warning", json_string("Deprecated use of 'listener' ptype, update to the new 'subscriber' ASAP"));
					session->participant_type = janus_videoroom_p_type_subscriber;
					JANUS_LOG(LOG_VERB, "Preparing JSON event as a reply\n");
					/* Negotiate by sending the selected publisher SDP back */
					janus_mutex_lock(&publisher->subscribers_mutex);
					if(publisher->sdp != NULL) {
						/* Check if there's something the original SDP has that we should remove */
						char *sdp = publisher->sdp;
						janus_sdp *offer = janus_sdp_parse(publisher->sdp, NULL, 0);
						subscriber->sdp = offer;
						session->sdp_version = 1;
						subscriber->sdp->o_version = session->sdp_version;
						if((publisher->audio && !subscriber->audio_offered) ||
								(publisher->video && !subscriber->video_offered) ||
								(publisher->data && !subscriber->data_offered)) {
							JANUS_LOG(LOG_VERB, "Munging SDP offer to adapt it to the subscriber's requirements\n");
							if(publisher->audio && !subscriber->audio_offered)
								janus_sdp_mline_remove(offer, JANUS_SDP_AUDIO);
							if(publisher->video && !subscriber->video_offered)
								janus_sdp_mline_remove(offer, JANUS_SDP_VIDEO);
							if(publisher->data && !subscriber->data_offered)
								janus_sdp_mline_remove(offer, JANUS_SDP_APPLICATION);
						}
						sdp = janus_sdp_write(offer);
						json_t *jsep = json_pack("{ssss}", "type", "offer", "sdp", sdp);
						g_free(sdp);
						janus_mutex_unlock(&publisher->subscribers_mutex);
						/* How long will the gateway take to push the event? */
						g_atomic_int_set(&session->hangingup, 0);
						gint64 start = janus_get_monotonic_time();
						int res = gateway->push_event(msg->handle, &janus_videoroom_plugin, msg->transaction, event, jsep);
						JANUS_LOG(LOG_VERB, "  >> Pushing event: %d (took %"SCNu64" us)\n", res, janus_get_monotonic_time()-start);
						json_decref(event);
						json_decref(jsep);
						janus_videoroom_message_free(msg);
						/* Also notify event handlers */
						if(notify_events && gateway->events_is_enabled()) {
							json_t *info = json_object();
							json_object_set_new(info, "event", json_string("subscribing"));
							json_object_set_new(info, "room", json_integer(subscriber->room->room_id));
							json_object_set_new(info, "feed", json_integer(feed_id));
							json_object_set_new(info, "private_id", json_integer(pvt_id));
							gateway->notify_event(&janus_videoroom_plugin, session->handle, info);
						}
						continue;
					}
					janus_mutex_unlock(&publisher->subscribers_mutex);
				}
			} else {
				JANUS_LOG(LOG_ERR, "Invalid element (ptype)\n");
				error_code = JANUS_VIDEOROOM_ERROR_INVALID_ELEMENT;
				g_snprintf(error_cause, 512, "Invalid element (ptype)");
				goto error;
			}
		} else if(session->participant_type == janus_videoroom_p_type_publisher) {
			/* Handle this publisher */
			participant = janus_videoroom_session_get_publisher(session);
			if(participant == NULL) {
				JANUS_LOG(LOG_ERR, "Invalid participant instance\n");
				error_code = JANUS_VIDEOROOM_ERROR_UNKNOWN_ERROR;
				g_snprintf(error_cause, 512, "Invalid participant instance");
				goto error;
			}
			if(participant->room == NULL) {
				janus_refcount_decrease(&participant->ref);
				JANUS_LOG(LOG_ERR, "No such room\n");
				error_code = JANUS_VIDEOROOM_ERROR_NO_SUCH_ROOM;
				g_snprintf(error_cause, 512, "No such room");
				goto error;
			}
			if(!strcasecmp(request_text, "join") || !strcasecmp(request_text, "joinandconfigure")) {
				janus_refcount_decrease(&participant->ref);
				JANUS_LOG(LOG_ERR, "Already in as a publisher on this handle\n");
				error_code = JANUS_VIDEOROOM_ERROR_ALREADY_JOINED;
				g_snprintf(error_cause, 512, "Already in as a publisher on this handle");
				goto error;
			} else if(!strcasecmp(request_text, "configure") || !strcasecmp(request_text, "publish")) {
				if(!strcasecmp(request_text, "publish") && participant->sdp) {
					janus_refcount_decrease(&participant->ref);
					JANUS_LOG(LOG_ERR, "Can't publish, already published\n");
					error_code = JANUS_VIDEOROOM_ERROR_ALREADY_PUBLISHED;
					g_snprintf(error_cause, 512, "Can't publish, already published");
					goto error;
				}
				if(participant->kicked) {
					janus_refcount_decrease(&participant->ref);
					JANUS_LOG(LOG_ERR, "Unauthorized, you have been kicked\n");
					error_code = JANUS_VIDEOROOM_ERROR_UNAUTHORIZED;
					g_snprintf(error_cause, 512, "Unauthorized, you have been kicked");
					goto error;
				}
				/* Configure (or publish a new feed) audio/video/bitrate for this publisher */
				JANUS_VALIDATE_JSON_OBJECT(root, publish_parameters,
					error_code, error_cause, TRUE,
					JANUS_VIDEOROOM_ERROR_MISSING_ELEMENT, JANUS_VIDEOROOM_ERROR_INVALID_ELEMENT);
				if(error_code != 0) {
					janus_refcount_decrease(&participant->ref);
					goto error;
				}
				json_t *audio = json_object_get(root, "audio");
				json_t *audiocodec = json_object_get(root, "audiocodec");
				json_t *video = json_object_get(root, "video");
				json_t *videocodec = json_object_get(root, "videocodec");
				json_t *data = json_object_get(root, "data");
				json_t *bitrate = json_object_get(root, "bitrate");
				json_t *record = json_object_get(root, "record");
				json_t *recfile = json_object_get(root, "filename");
				json_t *display = json_object_get(root, "display");
				json_t *update = json_object_get(root, "update");
				if(audio) {
					gboolean audio_active = json_is_true(audio);
					if(session->started && audio_active && !participant->audio_active) {
						/* Audio was just resumed, try resetting the RTP headers for viewers */
						janus_mutex_lock(&participant->subscribers_mutex);
						GSList *ps = participant->subscribers;
						while(ps) {
							janus_videoroom_subscriber *l = (janus_videoroom_subscriber *)ps->data;
							if(l)
								l->context.a_seq_reset = TRUE;
							ps = ps->next;
						}
						janus_mutex_unlock(&participant->subscribers_mutex);
					}
					participant->audio_active = audio_active;
					JANUS_LOG(LOG_VERB, "Setting audio property: %s (room %"SCNu64", user %"SCNu64")\n", participant->audio_active ? "true" : "false", participant->room_id, participant->user_id);
				}
				if(audiocodec && json_string_value(json_object_get(msg->jsep, "sdp")) != NULL) {
					/* The participant would like to use an audio codec in particular */
					janus_videoroom_audiocodec acodec = janus_videoroom_audiocodec_from_name(json_string_value(audiocodec));
					if(acodec == JANUS_VIDEOROOM_NOAUDIO ||
							(acodec != participant->room->acodec[0] &&
							acodec != participant->room->acodec[1] &&
							acodec != participant->room->acodec[2])) {
						JANUS_LOG(LOG_ERR, "Participant asked for audio codec '%s', but it's not allowed (room %"SCNu64", user %"SCNu64")\n",
							json_string_value(audiocodec), participant->room_id, participant->user_id);
						janus_refcount_decrease(&participant->ref);
						error_code = JANUS_VIDEOROOM_ERROR_INVALID_ELEMENT;
						g_snprintf(error_cause, 512, "Audio codec unavailable in this room");
						goto error;
					}
					participant->acodec = acodec;
					JANUS_LOG(LOG_VERB, "Participant asked for audio codec '%s' (room %"SCNu64", user %"SCNu64")\n",
						json_string_value(audiocodec), participant->room_id, participant->user_id);
				}
				if(video) {
					gboolean video_active = json_is_true(video);
					if(session->started && video_active && !participant->video_active) {
						/* Video was just resumed, try resetting the RTP headers for viewers */
						janus_mutex_lock(&participant->subscribers_mutex);
						GSList *ps = participant->subscribers;
						while(ps) {
							janus_videoroom_subscriber *l = (janus_videoroom_subscriber *)ps->data;
							if(l)
								l->context.v_seq_reset = TRUE;
							ps = ps->next;
						}
						janus_mutex_unlock(&participant->subscribers_mutex);
					}
					participant->video_active = video_active;
					JANUS_LOG(LOG_VERB, "Setting video property: %s (room %"SCNu64", user %"SCNu64")\n", participant->video_active ? "true" : "false", participant->room_id, participant->user_id);
				}
				if(videocodec && json_string_value(json_object_get(msg->jsep, "sdp")) != NULL) {
					/* The participant would like to use a video codec in particular */
					janus_videoroom_videocodec vcodec = janus_videoroom_videocodec_from_name(json_string_value(videocodec));
					if(vcodec == JANUS_VIDEOROOM_NOVIDEO ||
							(vcodec != participant->room->vcodec[0] &&
							vcodec != participant->room->vcodec[1] &&
							vcodec != participant->room->vcodec[2])) {
						JANUS_LOG(LOG_ERR, "Participant asked for video codec '%s', but it's not allowed (room %"SCNu64", user %"SCNu64")\n",
							json_string_value(videocodec), participant->room_id, participant->user_id);
						janus_refcount_decrease(&participant->ref);
						error_code = JANUS_VIDEOROOM_ERROR_INVALID_ELEMENT;
						g_snprintf(error_cause, 512, "Video codec unavailable in this room");
						goto error;
					}
					participant->vcodec = vcodec;
					JANUS_LOG(LOG_VERB, "Participant asked for video codec '%s' (room %"SCNu64", user %"SCNu64")\n",
						json_string_value(videocodec), participant->room_id, participant->user_id);
				}
				if(data) {
					gboolean data_active = json_is_true(data);
					participant->data_active = data_active;
					JANUS_LOG(LOG_VERB, "Setting data property: %s (room %"SCNu64", user %"SCNu64")\n", participant->data_active ? "true" : "false", participant->room_id, participant->user_id);
				}
				if(bitrate) {
					participant->bitrate = json_integer_value(bitrate);
					JANUS_LOG(LOG_VERB, "Setting video bitrate: %"SCNu32" (room %"SCNu64", user %"SCNu64")\n", participant->bitrate, participant->room_id, participant->user_id);
					/* Send a new REMB */
					if(session->started)
						participant->remb_latest = janus_get_monotonic_time();
					char rtcpbuf[24];
					janus_rtcp_remb((char *)(&rtcpbuf), 24, participant->bitrate);
					gateway->relay_rtcp(msg->handle, 1, rtcpbuf, 24);
				}
				janus_mutex_lock(&participant->rec_mutex);
				gboolean prev_recording_active = participant->recording_active;
				if(record) {
					participant->recording_active = json_is_true(record);
					JANUS_LOG(LOG_VERB, "Setting record property: %s (room %"SCNu64", user %"SCNu64")\n", participant->recording_active ? "true" : "false", participant->room_id, participant->user_id);
				}
				if(recfile) {
					participant->recording_base = g_strdup(json_string_value(recfile));
					JANUS_LOG(LOG_VERB, "Setting recording basename: %s (room %"SCNu64", user %"SCNu64")\n", participant->recording_base, participant->room_id, participant->user_id);
				}
				/* Do we need to do something with the recordings right now? */
				if(participant->recording_active != prev_recording_active) {
					/* Something changed */
					if(!participant->recording_active) {
						/* Not recording (anymore?) */
						janus_videoroom_recorder_close(participant);
					} else if(participant->recording_active && participant->sdp) {
						/* We've started recording, send a PLI/FIR and go on */
						janus_videoroom_recorder_create(
							participant, strstr(participant->sdp, "m=audio") != NULL,
							strstr(participant->sdp, "m=video") != NULL,
							strstr(participant->sdp, "m=application") != NULL);
						if(strstr(participant->sdp, "m=video")) {
							/* Send a FIR */
							char buf[20];
							memset(buf, 0, 20);
							janus_rtcp_fir((char *)&buf, 20, &participant->fir_seq);
							JANUS_LOG(LOG_VERB, "Recording video, sending FIR to %"SCNu64" (%s)\n",
								participant->user_id, participant->display ? participant->display : "??");
							gateway->relay_rtcp(participant->session->handle, 1, buf, 20);
							/* Send a PLI too, just in case... */
							janus_rtcp_pli((char *)&buf, 12);
							JANUS_LOG(LOG_VERB, "Recording video, sending PLI to %"SCNu64" (%s)\n",
								participant->user_id, participant->display ? participant->display : "??");
							gateway->relay_rtcp(participant->session->handle, 1, buf, 12);
							/* Update the time of when we last sent a keyframe request */
							participant->fir_latest = janus_get_monotonic_time();
						}
					}
				}
				janus_mutex_unlock(&participant->rec_mutex);
				if(display) {
					janus_mutex_lock(&participant->room->mutex);
					char *old_display = participant->display;
					char *new_display = g_strdup(json_string_value(display));
					participant->display = new_display;
					g_free(old_display);
					json_t *display_event = json_object();
					json_object_set_new(display_event, "videoroom", json_string("event"));
					json_object_set_new(display_event, "id", json_integer(participant->user_id));
					json_object_set_new(display_event, "display", json_string(participant->display));
					if(participant->room && !participant->room->destroyed) {
						janus_videoroom_notify_participants(participant, display_event);
					}
					janus_mutex_unlock(&participant->room->mutex);
					json_decref(display_event);
				}
				/* A renegotiation may be taking place */
				gboolean do_update = update ? json_is_true(update) : FALSE;
				if(do_update && !sdp_update) {
					JANUS_LOG(LOG_WARN, "Got an 'update' request, but no SDP update? Ignoring...\n");
				}
				/* Done */
				event = json_object();
				json_object_set_new(event, "videoroom", json_string("event"));
				json_object_set_new(event, "room", json_integer(participant->room_id));
				json_object_set_new(event, "configured", json_string("ok"));
				/* Also notify event handlers */
				if(notify_events && gateway->events_is_enabled()) {
					json_t *info = json_object();
					json_object_set_new(info, "event", json_string("configured"));
					json_object_set_new(info, "room", json_integer(participant->room_id));
					json_object_set_new(info, "id", json_integer(participant->user_id));
					json_object_set_new(info, "audio_active", participant->audio_active ? json_true() : json_false());
					json_object_set_new(info, "video_active", participant->video_active ? json_true() : json_false());
					json_object_set_new(info, "data_active", participant->data_active ? json_true() : json_false());
					json_object_set_new(info, "bitrate", json_integer(participant->bitrate));
					if(participant->arc || participant->vrc || participant->drc) {
						json_t *recording = json_object();
						if(participant->arc && participant->arc->filename)
							json_object_set_new(recording, "audio", json_string(participant->arc->filename));
						if(participant->vrc && participant->vrc->filename)
							json_object_set_new(recording, "video", json_string(participant->vrc->filename));
						if(participant->drc && participant->drc->filename)
							json_object_set_new(recording, "data", json_string(participant->drc->filename));
						json_object_set_new(info, "recording", recording);
					}
					gateway->notify_event(&janus_videoroom_plugin, session->handle, info);
				}
			} else if(!strcasecmp(request_text, "unpublish")) {
				/* This participant wants to unpublish */
				if(!participant->sdp) {
					janus_refcount_decrease(&participant->ref);
					JANUS_LOG(LOG_ERR, "Can't unpublish, not published\n");
					error_code = JANUS_VIDEOROOM_ERROR_NOT_PUBLISHED;
					g_snprintf(error_cause, 512, "Can't unpublish, not published");
					goto error;
				}
				/* Tell the core to tear down the PeerConnection, hangup_media will do the rest */
				janus_videoroom_hangup_media(session->handle);
				gateway->close_pc(session->handle);
				/* Done */
				event = json_object();
				json_object_set_new(event, "videoroom", json_string("event"));
				json_object_set_new(event, "room", json_integer(participant->room_id));
				json_object_set_new(event, "unpublished", json_string("ok"));
			} else if(!strcasecmp(request_text, "leave")) {
				/* Prepare an event to confirm the request */
				event = json_object();
				json_object_set_new(event, "videoroom", json_string("event"));
				json_object_set_new(event, "room", json_integer(participant->room_id));
				json_object_set_new(event, "leaving", json_string("ok"));
				/* This publisher is leaving, tell everybody */
				janus_videoroom_leave_or_unpublish(participant, TRUE, FALSE);
				/* Done */
				participant->audio_active = FALSE;
				participant->video_active = FALSE;
				participant->data_active = FALSE;
				session->started = FALSE;
				//~ session->destroy = TRUE;
			} else {
				janus_refcount_decrease(&participant->ref);
				JANUS_LOG(LOG_ERR, "Unknown request '%s'\n", request_text);
				error_code = JANUS_VIDEOROOM_ERROR_INVALID_REQUEST;
				g_snprintf(error_cause, 512, "Unknown request '%s'", request_text);
				goto error;
			}
			janus_refcount_decrease(&participant->ref);
		} else if(session->participant_type == janus_videoroom_p_type_subscriber) {
			/* Handle this subscriber */
			janus_videoroom_subscriber *subscriber = (janus_videoroom_subscriber *)session->participant;
			if(subscriber == NULL) {
				JANUS_LOG(LOG_ERR, "Invalid subscriber instance\n");
				error_code = JANUS_VIDEOROOM_ERROR_UNKNOWN_ERROR;
				g_snprintf(error_cause, 512, "Invalid subscriber instance");
				goto error;
			}
			if(subscriber->room == NULL) {
				JANUS_LOG(LOG_ERR, "No such room\n");
				error_code = JANUS_VIDEOROOM_ERROR_NO_SUCH_ROOM;
				g_snprintf(error_cause, 512, "No such room");
				goto error;
			}
			if(!strcasecmp(request_text, "join")) {
				JANUS_LOG(LOG_ERR, "Already in as a subscriber on this handle\n");
				error_code = JANUS_VIDEOROOM_ERROR_ALREADY_JOINED;
				g_snprintf(error_cause, 512, "Already in as a subscriber on this handle");
				goto error;
			} else if(!strcasecmp(request_text, "start")) {
				/* Start/restart receiving the publisher streams */
				subscriber->paused = FALSE;
				event = json_object();
				json_object_set_new(event, "videoroom", json_string("event"));
				json_object_set_new(event, "room", json_integer(subscriber->room_id));
				json_object_set_new(event, "started", json_string("ok"));
			} else if(!strcasecmp(request_text, "configure")) {
				JANUS_VALIDATE_JSON_OBJECT(root, configure_parameters,
					error_code, error_cause, TRUE,
					JANUS_VIDEOROOM_ERROR_MISSING_ELEMENT, JANUS_VIDEOROOM_ERROR_INVALID_ELEMENT);
				if(error_code != 0)
					goto error;
				if(subscriber->kicked) {
					JANUS_LOG(LOG_ERR, "Unauthorized, you have been kicked\n");
					error_code = JANUS_VIDEOROOM_ERROR_UNAUTHORIZED;
					g_snprintf(error_cause, 512, "Unauthorized, you have been kicked");
					goto error;
				}
				json_t *audio = json_object_get(root, "audio");
				json_t *video = json_object_get(root, "video");
				json_t *data = json_object_get(root, "data");
				json_t *restart = json_object_get(root, "restart");
				json_t *update = json_object_get(root, "update");
				json_t *spatial = json_object_get(root, "spatial_layer");
				json_t *temporal = json_object_get(root, "temporal_layer");
				json_t *sc_substream = json_object_get(root, "substream");
				if(json_integer_value(sc_substream) > 2) {
					JANUS_LOG(LOG_ERR, "Invalid element (substream should be 0, 1 or 2)\n");
					error_code = JANUS_VIDEOROOM_ERROR_INVALID_ELEMENT;
					g_snprintf(error_cause, 512, "Invalid value (substream should be 0, 1 or 2)");
					goto error;
				}
				json_t *sc_temporal = json_object_get(root, "temporal");
				if(json_integer_value(sc_temporal) > 2) {
					JANUS_LOG(LOG_ERR, "Invalid element (temporal should be 0, 1 or 2)\n");
					error_code = JANUS_VIDEOROOM_ERROR_INVALID_ELEMENT;
					g_snprintf(error_cause, 512, "Invalid value (temporal should be 0, 1 or 2)");
					goto error;
				}
				/* Update the audio/video/data flags, if set */
				janus_videoroom_publisher *publisher = subscriber->feed;
				if(publisher) {
					if(audio && publisher->audio && subscriber->audio_offered)
						subscriber->audio = json_is_true(audio);
					if(video && publisher->video && subscriber->video_offered) {
						subscriber->video = json_is_true(video);
						if(subscriber->video) {
							/* Send a FIR */
							char buf[20];
							janus_rtcp_fir((char *)&buf, 20, &publisher->fir_seq);
							JANUS_LOG(LOG_VERB, "Restoring video for subscriber, sending FIR to %"SCNu64" (%s)\n", publisher->user_id, publisher->display ? publisher->display : "??");
							gateway->relay_rtcp(publisher->session->handle, 1, buf, 20);
							/* Send a PLI too, just in case... */
							janus_rtcp_pli((char *)&buf, 12);
							JANUS_LOG(LOG_VERB, "Restoring video for subscriber, sending PLI to %"SCNu64" (%s)\n", publisher->user_id, publisher->display ? publisher->display : "??");
							gateway->relay_rtcp(publisher->session->handle, 1, buf, 12);
							/* Update the time of when we last sent a keyframe request */
							publisher->fir_latest = janus_get_monotonic_time();
						}
					}
					if(data && publisher->data && subscriber->data_offered)
						subscriber->data = json_is_true(data);
					/* Check if a simulcasting-related request is involved */
					if(sc_substream && publisher->ssrc[0] != 0) {
						subscriber->substream_target = json_integer_value(sc_substream);
						JANUS_LOG(LOG_VERB, "Setting video SSRC to let through (simulcast): %"SCNu32" (index %d, was %d)\n",
							publisher->ssrc[subscriber->substream], subscriber->substream_target, subscriber->substream);
						if(subscriber->substream_target == subscriber->substream) {
							/* No need to do anything, we're already getting the right substream, so notify the user */
							json_t *event = json_object();
							json_object_set_new(event, "videoroom", json_string("event"));
							json_object_set_new(event, "room", json_integer(subscriber->room_id));
							json_object_set_new(event, "substream", json_integer(subscriber->substream));
							gateway->push_event(msg->handle, &janus_videoroom_plugin, NULL, event, NULL);
							json_decref(event);
						} else {
							/* Send a FIR */
							char buf[20];
							janus_rtcp_fir((char *)&buf, 20, &publisher->fir_seq);
							JANUS_LOG(LOG_VERB, "Simulcasting substream change, sending FIR to %"SCNu64" (%s)\n", publisher->user_id, publisher->display ? publisher->display : "??");
							gateway->relay_rtcp(publisher->session->handle, 1, buf, 20);
							/* Send a PLI too, just in case... */
							janus_rtcp_pli((char *)&buf, 12);
							JANUS_LOG(LOG_VERB, "Simulcasting substream change, sending PLI to %"SCNu64" (%s)\n", publisher->user_id, publisher->display ? publisher->display : "??");
							gateway->relay_rtcp(publisher->session->handle, 1, buf, 12);
							/* Update the time of when we last sent a keyframe request */
							publisher->fir_latest = janus_get_monotonic_time();
						}
					}
					if(sc_temporal && publisher->ssrc[0] != 0) {
						subscriber->templayer_target = json_integer_value(sc_temporal);
						JANUS_LOG(LOG_VERB, "Setting video temporal layer to let through (simulcast): %d (was %d)\n",
							subscriber->templayer_target, subscriber->templayer);
						if(subscriber->templayer_target == subscriber->templayer) {
							/* No need to do anything, we're already getting the right temporal, so notify the user */
							json_t *event = json_object();
							json_object_set_new(event, "videoroom", json_string("event"));
							json_object_set_new(event, "room", json_integer(subscriber->room_id));
							json_object_set_new(event, "temporal", json_integer(subscriber->templayer));
							gateway->push_event(msg->handle, &janus_videoroom_plugin, NULL, event, NULL);
							json_decref(event);
						} else {
							/* Send a FIR */
							char buf[20];
							janus_rtcp_fir((char *)&buf, 20, &publisher->fir_seq);
							JANUS_LOG(LOG_VERB, "Simulcasting temporal layer change, sending FIR to %"SCNu64" (%s)\n", publisher->user_id, publisher->display ? publisher->display : "??");
							gateway->relay_rtcp(publisher->session->handle, 1, buf, 20);
							/* Send a PLI too, just in case... */
							janus_rtcp_pli((char *)&buf, 12);
							JANUS_LOG(LOG_VERB, "Simulcasting temporal layer change, sending PLI to %"SCNu64" (%s)\n", publisher->user_id, publisher->display ? publisher->display : "??");
							gateway->relay_rtcp(publisher->session->handle, 1, buf, 12);
							/* Update the time of when we last sent a keyframe request */
							publisher->fir_latest = janus_get_monotonic_time();
						}
					}
				}
				if(subscriber->room->do_svc) {
					/* Also check if the viewer is trying to configure a layer change */
					if(spatial) {
						int spatial_layer = json_integer_value(spatial);
						if(spatial_layer > 1) {
							JANUS_LOG(LOG_WARN, "Spatial layer higher than 1, will probably be ignored\n");
						}
						if(spatial_layer == subscriber->spatial_layer) {
							/* No need to do anything, we're already getting the right spatial layer, so notify the user */
							json_t *event = json_object();
							json_object_set_new(event, "videoroom", json_string("event"));
							json_object_set_new(event, "room", json_integer(subscriber->room_id));
							json_object_set_new(event, "spatial_layer", json_integer(subscriber->spatial_layer));
							gateway->push_event(msg->handle, &janus_videoroom_plugin, NULL, event, NULL);
							json_decref(event);
						} else if(spatial_layer != subscriber->target_spatial_layer) {
							/* Send a FIR to the new RTP forward publisher */
							char buf[20];
							janus_rtcp_fir((char *)&buf, 20, &publisher->fir_seq);
							JANUS_LOG(LOG_VERB, "Need to downscale spatially, sending FIR to %"SCNu64" (%s)\n", publisher->user_id, publisher->display ? publisher->display : "??");
							gateway->relay_rtcp(publisher->session->handle, 1, buf, 20);
							/* Send a PLI too, just in case... */
							janus_rtcp_pli((char *)&buf, 12);
							JANUS_LOG(LOG_VERB, "Need to downscale spatially, sending PLI to %"SCNu64" (%s)\n", publisher->user_id, publisher->display ? publisher->display : "??");
							gateway->relay_rtcp(publisher->session->handle, 1, buf, 12);
							/* Update the time of when we last sent a keyframe request */
							publisher->fir_latest = janus_get_monotonic_time();
						}
						subscriber->target_spatial_layer = spatial_layer;
					}
					if(temporal) {
						int temporal_layer = json_integer_value(temporal);
						if(temporal_layer > 2) {
							JANUS_LOG(LOG_WARN, "Temporal layer higher than 2, will probably be ignored\n");
						}
						if(temporal_layer == subscriber->temporal_layer) {
							/* No need to do anything, we're already getting the right temporal layer, so notify the user */
							json_t *event = json_object();
							json_object_set_new(event, "videoroom", json_string("event"));
							json_object_set_new(event, "room", json_integer(subscriber->room_id));
							json_object_set_new(event, "temporal_layer", json_integer(subscriber->temporal_layer));
							gateway->push_event(msg->handle, &janus_videoroom_plugin, NULL, event, NULL);
							json_decref(event);
						}
						subscriber->target_temporal_layer = temporal_layer;
					}
				}
				event = json_object();
				json_object_set_new(event, "videoroom", json_string("event"));
				json_object_set_new(event, "room", json_integer(subscriber->room_id));
				json_object_set_new(event, "configured", json_string("ok"));
				/* The user may be interested in an ICE restart */
				gboolean do_restart = restart ? json_is_true(restart) : FALSE;
				gboolean do_update = update ? json_is_true(update) : FALSE;
				if(sdp_update || do_restart || do_update) {
					/* Negotiate by sending the selected publisher SDP back, and/or force an ICE restart */
					if(publisher->sdp != NULL) {
						char temp_error[512];
						JANUS_LOG(LOG_VERB, "Munging SDP offer (update) to adapt it to the subscriber's requirements\n");
						janus_sdp *offer = janus_sdp_parse(publisher->sdp, temp_error, sizeof(temp_error));
						if(publisher->audio && !subscriber->audio_offered)
							janus_sdp_mline_remove(offer, JANUS_SDP_AUDIO);
						if(publisher->video && !subscriber->video_offered)
							janus_sdp_mline_remove(offer, JANUS_SDP_VIDEO);
						if(publisher->data && !subscriber->data_offered)
							janus_sdp_mline_remove(offer, JANUS_SDP_APPLICATION);
						/* This is an update, check if we need to update */
						janus_sdp_mtype mtype[3] = { JANUS_SDP_AUDIO, JANUS_SDP_VIDEO, JANUS_SDP_APPLICATION };
						int i=0;
						for(i=0; i<3; i++) {
							janus_sdp_mline *m = janus_sdp_mline_find(subscriber->sdp, mtype[i]);
							janus_sdp_mline *m_new = janus_sdp_mline_find(offer, mtype[i]);
							if(m != NULL && m->port > 0 && m->port != JANUS_SDP_INACTIVE) {
								/* We have such an m-line and it's active, should it be changed? */
								if(m_new == NULL || m_new->port == 0 || m_new->direction == JANUS_SDP_INACTIVE) {
									/* Turn the m-line to inactive */
									m->port = 0;
									m->direction = JANUS_SDP_INACTIVE;
								}
							} else {
								/* We don't have such an m-line or it's disabled, should it be added/enabled? */
								if(m_new != NULL && m_new->port > 0 && m_new->direction != JANUS_SDP_INACTIVE) {
									if(m != NULL) {
										m->port = m_new->port;
										m->direction = m_new->direction;
									} else {
										/* Add the new m-line */
										m = janus_sdp_mline_create(m_new->type, m_new->port, m_new->proto, m_new->direction);
										subscriber->sdp->m_lines = g_list_append(subscriber->sdp->m_lines, m);
									}
									/* Copy/replace the other properties */
									m->c_ipv4 = m_new->c_ipv4;
									if(m_new->c_addr && (m->c_addr == NULL || strcmp(m->c_addr, m_new->c_addr))) {
										g_free(m->c_addr);
										m->c_addr = g_strdup(m_new->c_addr);
									}
									if(m_new->b_name && (m->b_name == NULL || strcmp(m->b_name, m_new->b_name))) {
										g_free(m->b_name);
										m->b_name = g_strdup(m_new->b_name);
									}
									m->b_value = m_new->b_value;
									g_list_free_full(m->fmts, (GDestroyNotify)g_free);
									m->fmts = NULL;
									GList *fmts = m_new->fmts;
									while(fmts) {
										char *fmt = (char *)fmts->data;
										if(fmt)
											m->fmts = g_list_append(m->fmts,g_strdup(fmt));
										fmts = fmts->next;
									}
									g_list_free(m->ptypes);
									m->ptypes = g_list_copy(m_new->ptypes);
									g_list_free_full(m->attributes, (GDestroyNotify)janus_sdp_attribute_destroy);
									m->attributes = NULL;
									GList *attr = m_new->attributes;
									while(attr) {
										janus_sdp_attribute *a = (janus_sdp_attribute *)attr->data;
										janus_sdp_attribute_add_to_mline(m,
											janus_sdp_attribute_create(a->name, "%s", a->value));
										attr = attr->next;
									}
								}
							}
						}
						janus_sdp_destroy(offer);
						session->sdp_version++;
						subscriber->sdp->o_version = session->sdp_version;
						char *newsdp = janus_sdp_write(subscriber->sdp);
						JANUS_LOG(LOG_VERB, "Updating subscriber:\n%s\n", newsdp);
						json_t *jsep = json_pack("{ssss}", "type", "offer", "sdp", newsdp);
						if(do_restart)
							json_object_set_new(jsep, "restart", json_true());
						/* How long will the gateway take to push the event? */
						gint64 start = janus_get_monotonic_time();
						int res = gateway->push_event(msg->handle, &janus_videoroom_plugin, msg->transaction, event, jsep);
						JANUS_LOG(LOG_VERB, "  >> Pushing event: %d (took %"SCNu64" us)\n", res, janus_get_monotonic_time()-start);
						json_decref(event);
						json_decref(jsep);
						g_free(newsdp);
						/* Any update in the media directions? */
						subscriber->audio = publisher->audio;
						subscriber->video = publisher->video;
						subscriber->data = publisher->data;
						/* Done */
						janus_videoroom_message_free(msg);
						continue;
					}
				}
			} else if(!strcasecmp(request_text, "pause")) {
				/* Stop receiving the publisher streams for a while */
				subscriber->paused = TRUE;
				event = json_object();
				json_object_set_new(event, "videoroom", json_string("event"));
				json_object_set_new(event, "room", json_integer(subscriber->room_id));
				json_object_set_new(event, "paused", json_string("ok"));
			} else if(!strcasecmp(request_text, "switch")) {
				/* This subscriber wants to switch to a different publisher */
				JANUS_VALIDATE_JSON_OBJECT(root, subscriber_parameters,
					error_code, error_cause, TRUE,
					JANUS_VIDEOROOM_ERROR_MISSING_ELEMENT, JANUS_VIDEOROOM_ERROR_INVALID_ELEMENT);
				if(error_code != 0)
					goto error;
				json_t *feed = json_object_get(root, "feed");
				guint64 feed_id = json_integer_value(feed);
				json_t *audio = json_object_get(root, "audio");
				json_t *video = json_object_get(root, "video");
				json_t *data = json_object_get(root, "data");
				if(!subscriber->room) {
					JANUS_LOG(LOG_ERR, "Room Destroyed \n");
					error_code = JANUS_VIDEOROOM_ERROR_NO_SUCH_ROOM;
					g_snprintf(error_cause, 512, "No such room ");
					goto error;
				}
				if(g_atomic_int_get(&subscriber->destroyed)) {
					JANUS_LOG(LOG_ERR, "Room Destroyed (%"SCNu64")\n", subscriber->room_id);
					error_code = JANUS_VIDEOROOM_ERROR_NO_SUCH_ROOM;
					g_snprintf(error_cause, 512, "No such room (%"SCNu64")", subscriber->room_id);
					goto error;
				}
				janus_mutex_lock(&subscriber->room->mutex);
				janus_videoroom_publisher *publisher = g_hash_table_lookup(subscriber->room->participants, &feed_id);
				if(publisher == NULL || g_atomic_int_get(&publisher->destroyed) || publisher->sdp == NULL) {
					JANUS_LOG(LOG_ERR, "No such feed (%"SCNu64")\n", feed_id);
					error_code = JANUS_VIDEOROOM_ERROR_NO_SUCH_FEED;
					g_snprintf(error_cause, 512, "No such feed (%"SCNu64")", feed_id);
					janus_mutex_unlock(&subscriber->room->mutex);
					goto error;
				}
				janus_refcount_increase(&publisher->ref);
				janus_refcount_increase(&publisher->session->ref);
				janus_mutex_unlock(&subscriber->room->mutex);
				gboolean paused = subscriber->paused;
				subscriber->paused = TRUE;
				/* Unsubscribe from the previous publisher */
				janus_videoroom_publisher *prev_feed = subscriber->feed;
				if(prev_feed) {
					/* ... but make sure the codecs are compliant first */
					if(publisher->acodec != prev_feed->acodec || publisher->vcodec != prev_feed->vcodec) {
						janus_refcount_decrease(&publisher->session->ref);
						janus_refcount_decrease(&publisher->ref);
						subscriber->paused = paused;
						JANUS_LOG(LOG_ERR, "The two publishers are not using the same codecs, can't switch\n");
						error_code = JANUS_VIDEOROOM_ERROR_INVALID_SDP;
						g_snprintf(error_cause, 512, "The two publishers are not using the same codecs, can't switch");
						goto error;
					}
					/* Go on */
					janus_mutex_lock(&prev_feed->subscribers_mutex);
					prev_feed->subscribers = g_slist_remove(prev_feed->subscribers, subscriber);
					janus_mutex_unlock(&prev_feed->subscribers_mutex);
					janus_refcount_decrease(&prev_feed->session->ref);
					g_clear_pointer(&subscriber->feed, janus_videoroom_publisher_dereference);
				}
				/* Subscribe to the new one */
				subscriber->audio = audio ? json_is_true(audio) : TRUE;	/* True by default */
				if(!publisher->audio)
					subscriber->audio = FALSE;	/* ... unless the publisher isn't sending any audio */
				subscriber->video = video ? json_is_true(video) : TRUE;	/* True by default */
				if(!publisher->video)
					subscriber->video = FALSE;	/* ... unless the publisher isn't sending any video */
				subscriber->data = data ? json_is_true(data) : TRUE;	/* True by default */
				if(!publisher->data)
					subscriber->data = FALSE;	/* ... unless the publisher isn't sending any data */
				if(subscriber->room && subscriber->room->do_svc) {
					/* This subscriber belongs to a room where VP9 SVC has been enabled,
					 * let's assume we're interested in all layers for the time being */
					subscriber->spatial_layer = -1;
					subscriber->target_spatial_layer = 1;		/* FIXME Chrome sends 0 and 1 */
					subscriber->temporal_layer = -1;
					subscriber->target_temporal_layer = 2;	/* FIXME Chrome sends 0, 1 and 2 */
				}
				janus_mutex_lock(&publisher->subscribers_mutex);
				publisher->subscribers = g_slist_append(publisher->subscribers, subscriber);
				janus_mutex_unlock(&publisher->subscribers_mutex);
				subscriber->feed = publisher;
				/* Send a FIR to the new publisher */
				char buf[20];
				janus_rtcp_fir((char *)&buf, 20, &publisher->fir_seq);
				JANUS_LOG(LOG_VERB, "Switching existing subscriber to new publisher, sending FIR to %"SCNu64" (%s)\n", publisher->user_id, publisher->display ? publisher->display : "??");
				gateway->relay_rtcp(publisher->session->handle, 1, buf, 20);
				/* Send a PLI too, just in case... */
				janus_rtcp_pli((char *)&buf, 12);
				JANUS_LOG(LOG_VERB, "Switching existing subscriber to new publisher, sending PLI to %"SCNu64" (%s)\n", publisher->user_id, publisher->display ? publisher->display : "??");
				gateway->relay_rtcp(publisher->session->handle, 1, buf, 12);
				/* Update the time of when we last sent a keyframe request */
				publisher->fir_latest = janus_get_monotonic_time();
				/* Done */
				subscriber->paused = paused;
				event = json_object();
				json_object_set_new(event, "videoroom", json_string("event"));
				json_object_set_new(event, "switched", json_string("ok"));
				json_object_set_new(event, "room", json_integer(subscriber->room_id));
				json_object_set_new(event, "id", json_integer(feed_id));
				if(publisher->display)
					json_object_set_new(event, "display", json_string(publisher->display));
				/* Also notify event handlers */
				if(notify_events && gateway->events_is_enabled()) {
					json_t *info = json_object();
					json_object_set_new(info, "event", json_string("switched"));
					json_object_set_new(info, "room", json_integer(publisher->room_id));
					json_object_set_new(info, "feed", json_integer(publisher->user_id));
					gateway->notify_event(&janus_videoroom_plugin, session->handle, info);
				}
			} else if(!strcasecmp(request_text, "leave")) {
				guint64 room_id = subscriber && subscriber->room ? subscriber->room->room_id : 0;
				/* Tell the core to tear down the PeerConnection, hangup_media will do the rest */
				janus_videoroom_hangup_media(session->handle);
				gateway->close_pc(session->handle);
				/* Send an event back */
				event = json_object();
				json_object_set_new(event, "videoroom", json_string("event"));
				json_object_set_new(event, "room", json_integer(room_id));
				json_object_set_new(event, "left", json_string("ok"));
				session->started = FALSE;
			} else {
				JANUS_LOG(LOG_ERR, "Unknown request '%s'\n", request_text);
				error_code = JANUS_VIDEOROOM_ERROR_INVALID_REQUEST;
				g_snprintf(error_cause, 512, "Unknown request '%s'", request_text);
				goto error;
			}
		}

		/* Prepare JSON event */
		JANUS_LOG(LOG_VERB, "Preparing JSON event as a reply\n");
		/* Any SDP or update to handle? */
		const char *msg_sdp_type = json_string_value(json_object_get(msg->jsep, "type"));
		const char *msg_sdp = json_string_value(json_object_get(msg->jsep, "sdp"));
		json_t *msg_simulcast = json_object_get(msg->jsep, "simulcast");
		if(!msg_sdp) {
			/* No SDP to send */
			int ret = gateway->push_event(msg->handle, &janus_videoroom_plugin, msg->transaction, event, NULL);
			JANUS_LOG(LOG_VERB, "  >> %d (%s)\n", ret, janus_get_api_error(ret));
			json_decref(event);
		} else {
			/* Generate offer or answer */
			JANUS_LOG(LOG_VERB, "This is involving a negotiation (%s) as well:\n%s\n", msg_sdp_type, msg_sdp);
			if(sdp_update) {
				/* Renegotiation: make sure the user provided an offer, and send answer */
				JANUS_LOG(LOG_VERB, "  -- Updating existing publisher\n");
				session->sdp_version++;		/* This needs to be increased when it changes */
			} else {
				/* New PeerConnection */
				session->sdp_version = 1;	/* This needs to be increased when it changes */
				session->sdp_sessid = janus_get_real_time();
			}
			const char *type = NULL;
			if(!strcasecmp(msg_sdp_type, "offer")) {
				/* We need to answer */
				type = "answer";
			} else if(!strcasecmp(msg_sdp_type, "answer")) {
				/* We got an answer (from a subscriber?), no need to negotiate */
				g_atomic_int_set(&session->hangingup, 0);
				int ret = gateway->push_event(msg->handle, &janus_videoroom_plugin, msg->transaction, event, NULL);
				JANUS_LOG(LOG_VERB, "  >> %d (%s)\n", ret, janus_get_api_error(ret));
				json_decref(event);
				janus_videoroom_message_free(msg);
				continue;
			} else {
				/* TODO We don't support anything else right now... */
				JANUS_LOG(LOG_ERR, "Unknown SDP type '%s'\n", msg_sdp_type);
				error_code = JANUS_VIDEOROOM_ERROR_INVALID_SDP_TYPE;
				g_snprintf(error_cause, 512, "Unknown SDP type '%s'", msg_sdp_type);
				goto error;
			}
			if(session->participant_type != janus_videoroom_p_type_publisher) {
				/* We shouldn't be here, we always offer ourselves */
				JANUS_LOG(LOG_ERR, "Only publishers send offers\n");
				error_code = JANUS_VIDEOROOM_ERROR_INVALID_SDP_TYPE;
				g_snprintf(error_cause, 512, "Only publishers send offers");
				goto error;
			} else {
				/* This is a new publisher: is there room? */
				participant = janus_videoroom_session_get_publisher(session);
				janus_videoroom *videoroom = participant->room;
				int count = 0;
				GHashTableIter iter;
				gpointer value;
				if(!videoroom) {
					error_code = JANUS_VIDEOROOM_ERROR_NO_SUCH_ROOM;
					goto error;
				}
				if(g_atomic_int_get(&videoroom->destroyed)) {
					error_code = JANUS_VIDEOROOM_ERROR_NO_SUCH_ROOM;
					goto error;
				}
				janus_mutex_lock(&videoroom->mutex);
				g_hash_table_iter_init(&iter, videoroom->participants);
				while (!g_atomic_int_get(&videoroom->destroyed) && g_hash_table_iter_next(&iter, NULL, &value)) {
					janus_videoroom_publisher *p = value;
					if(p != participant && p->sdp)
						count++;
				}
				janus_mutex_unlock(&videoroom->mutex);
				if(count == videoroom->max_publishers) {
					participant->audio_active = FALSE;
					participant->video_active = FALSE;
					participant->data_active = FALSE;
					JANUS_LOG(LOG_ERR, "Maximum number of publishers (%d) already reached\n", videoroom->max_publishers);
					error_code = JANUS_VIDEOROOM_ERROR_PUBLISHERS_FULL;
					g_snprintf(error_cause, 512, "Maximum number of publishers (%d) already reached", videoroom->max_publishers);
					goto error;
				}
				/* Now prepare the SDP to give back */
				if(strstr(msg_sdp, "Mozilla")) {
					participant->firefox = TRUE;
				}
				/* Start by parsing the offer */
				char error_str[512];
				janus_sdp *offer = janus_sdp_parse(msg_sdp, error_str, sizeof(error_str));
				if(offer == NULL) {
					json_decref(event);
					JANUS_LOG(LOG_ERR, "Error parsing offer: %s\n", error_str);
					error_code = JANUS_VIDEOROOM_ERROR_INVALID_SDP;
					g_snprintf(error_cause, 512, "Error parsing offer: %s", error_str);
					goto error;
				}
				gboolean audio_level_extmap = FALSE, video_orient_extmap = FALSE, playout_delay_extmap = FALSE, transport_wide_cc_extmap = FALSE;
				janus_sdp_mdirection audio_level_mdir = JANUS_SDP_SENDRECV,
					video_orient_mdir = JANUS_SDP_SENDRECV,
					playout_delay_mdir = JANUS_SDP_SENDRECV;
				GList *temp = offer->m_lines;
				while(temp) {
					/* Which media are available? */
					janus_sdp_mline *m = (janus_sdp_mline *)temp->data;
					if(m->type == JANUS_SDP_AUDIO && m->port > 0 &&
							m->direction != JANUS_SDP_RECVONLY && m->direction != JANUS_SDP_INACTIVE) {
						participant->audio = TRUE;
					} else if(m->type == JANUS_SDP_VIDEO && m->port > 0 &&
							m->direction != JANUS_SDP_RECVONLY && m->direction != JANUS_SDP_INACTIVE) {
						participant->video = TRUE;
					} else if(m->type == JANUS_SDP_APPLICATION && m->port > 0) {
						participant->data = TRUE;
					}
					if(m->type == JANUS_SDP_AUDIO || m->type == JANUS_SDP_VIDEO) {
						/* Are the extmaps we care about there? */
						GList *ma = m->attributes;
						while(ma) {
							janus_sdp_attribute *a = (janus_sdp_attribute *)ma->data;
							if(a->value) {
								if(videoroom->audiolevel_ext && m->type == JANUS_SDP_AUDIO && strstr(a->value, JANUS_RTP_EXTMAP_AUDIO_LEVEL)) {
									participant->audio_level_extmap_id = atoi(a->value);
									audio_level_extmap = TRUE;
									audio_level_mdir = a->direction;
								} else if(videoroom->videoorient_ext && m->type == JANUS_SDP_VIDEO && strstr(a->value, JANUS_RTP_EXTMAP_VIDEO_ORIENTATION)) {
									participant->video_orient_extmap_id = atoi(a->value);
									video_orient_extmap = TRUE;
									video_orient_mdir = a->direction;
								} else if(videoroom->transport_wide_cc_ext && strstr(a->value, JANUS_RTP_EXTMAP_TRANSPORT_WIDE_CC)) {
									participant->transport_wide_cc_extmap_id = atoi(a->value);
									transport_wide_cc_extmap = TRUE;
								} else if(videoroom->playoutdelay_ext && m->type == JANUS_SDP_VIDEO && strstr(a->value, JANUS_RTP_EXTMAP_PLAYOUT_DELAY)) {
									participant->playout_delay_extmap_id = atoi(a->value);
									playout_delay_extmap = TRUE;
									playout_delay_mdir = a->direction;
								}
								
							}
							ma = ma->next;
						}
					}
					temp = temp->next;
				}
				/* Prepare an answer now: force the room codecs and recvonly on the Janus side */
				JANUS_LOG(LOG_VERB, "The publisher %s going to send an audio stream\n", participant->audio ? "is" : "is NOT");
				JANUS_LOG(LOG_VERB, "The publisher %s going to send a video stream\n", participant->video ? "is" : "is NOT");
				JANUS_LOG(LOG_VERB, "The publisher %s going to open a data channel\n", participant->data ? "is" : "is NOT");
				/* Check the codecs we can use, or the ones we should */
				if(participant->acodec == JANUS_VIDEOROOM_NOAUDIO) {
					int i=0;
					for(i=0; i<3; i++) {
						if(participant->room->acodec[i] == JANUS_VIDEOROOM_NOAUDIO)
							continue;
						if(janus_sdp_get_codec_pt(offer, janus_videoroom_audiocodec_name(participant->room->acodec[i])) != -1) {
							participant->acodec = participant->room->acodec[i];
							break;
						}
					}
				}
				JANUS_LOG(LOG_VERB, "The publisher is going to use the %s audio codec\n", janus_videoroom_audiocodec_name(participant->acodec));
				participant->audio_pt = janus_videoroom_audiocodec_pt(participant->acodec);
				if(participant->vcodec == JANUS_VIDEOROOM_NOVIDEO) {
					int i=0;
					for(i=0; i<3; i++) {
						if(participant->room->vcodec[i] == JANUS_VIDEOROOM_NOVIDEO)
							continue;
						if(janus_sdp_get_codec_pt(offer, janus_videoroom_videocodec_name(participant->room->vcodec[i])) != -1) {
							participant->vcodec = participant->room->vcodec[i];
							break;
						}
					}
				}
				JANUS_LOG(LOG_VERB, "The publisher is going to use the %s video codec\n", janus_videoroom_videocodec_name(participant->vcodec));
				participant->video_pt = janus_videoroom_videocodec_pt(participant->vcodec);
				janus_sdp *answer = janus_sdp_generate_answer(offer,
					JANUS_SDP_OA_AUDIO_CODEC, janus_videoroom_audiocodec_name(participant->acodec),
					JANUS_SDP_OA_AUDIO_DIRECTION, JANUS_SDP_RECVONLY,
					JANUS_SDP_OA_VIDEO_CODEC, janus_videoroom_videocodec_name(participant->vcodec),
					JANUS_SDP_OA_VIDEO_DIRECTION, JANUS_SDP_RECVONLY,
					JANUS_SDP_OA_DONE);
				janus_sdp_destroy(offer);
				/* Replace the session name */
				g_free(answer->s_name);
				char s_name[100];
				g_snprintf(s_name, sizeof(s_name), "VideoRoom %"SCNu64, videoroom->room_id);
				answer->s_name = g_strdup(s_name);
				/* Which media are REALLY available? (some may have been rejected) */
				participant->audio = FALSE;
				participant->video = FALSE;
				participant->data = FALSE;
				temp = answer->m_lines;
				while(temp) {
					janus_sdp_mline *m = (janus_sdp_mline *)temp->data;
					if(m->type == JANUS_SDP_AUDIO && m->port > 0 && m->direction != JANUS_SDP_INACTIVE) {
						participant->audio = TRUE;
					} else if(m->type == JANUS_SDP_VIDEO && m->port > 0 && m->direction != JANUS_SDP_INACTIVE) {
						participant->video = TRUE;
					} else if(m->type == JANUS_SDP_APPLICATION && m->port > 0) {
						participant->data = TRUE;
					}
					temp = temp->next;
				}
				JANUS_LOG(LOG_VERB, "Per the answer, the publisher %s going to send an audio stream\n", participant->audio ? "is" : "is NOT");
				JANUS_LOG(LOG_VERB, "Per the answer, the publisher %s going to send a video stream\n", participant->video ? "is" : "is NOT");
				JANUS_LOG(LOG_VERB, "Per the answer, the publisher %s going to open a data channel\n", participant->data ? "is" : "is NOT");
				/* Update the event with info on the codecs that we'll be handling */
				if(event) {
					if(participant->audio)
						json_object_set_new(event, "audio_codec", json_string(janus_videoroom_audiocodec_name(participant->acodec)));
					if(participant->video)
						json_object_set_new(event, "video_codec", json_string(janus_videoroom_videocodec_name(participant->vcodec)));
				}
				/* Also add a bandwidth SDP attribute if we're capping the bitrate in the room */
				if(participant->firefox) {	/* Don't add any b=AS attribute for Chrome */
					janus_sdp_mline *m = janus_sdp_mline_find(answer, JANUS_SDP_VIDEO);
					if(m != NULL && videoroom->bitrate > 0) {
						m->b_name = g_strdup("AS");
						m->b_value = (int)(videoroom->bitrate/1000);
					}
				}
				/* Add the extmap attributes, if needed */
				if(audio_level_extmap) {
					/* First of all, let's check if the extmap attribute had a direction */
					const char *direction = NULL;
					switch(audio_level_mdir) {
						case JANUS_SDP_SENDONLY:
							direction = "/recvonly";
							break;
						case JANUS_SDP_RECVONLY:
						case JANUS_SDP_INACTIVE:
							direction = "/inactive";
							break;
						default:
							direction = "";
							break;
					}
					janus_sdp_attribute *a = janus_sdp_attribute_create("extmap",
						"%d%s %s\r\n", participant->audio_level_extmap_id, direction, JANUS_RTP_EXTMAP_AUDIO_LEVEL);
					janus_sdp_attribute_add_to_mline(janus_sdp_mline_find(answer, JANUS_SDP_AUDIO), a);
				}
				if(video_orient_extmap) {
					/* First of all, let's check if the extmap attribute had a direction */
					const char *direction = NULL;
					switch(video_orient_mdir) {
						case JANUS_SDP_SENDONLY:
							direction = "/recvonly";
							break;
						case JANUS_SDP_RECVONLY:
						case JANUS_SDP_INACTIVE:
							direction = "/inactive";
							break;
						default:
							direction = "";
							break;
					}
					janus_sdp_attribute *a = janus_sdp_attribute_create("extmap",
						"%d%s %s\r\n", participant->video_orient_extmap_id, direction, JANUS_RTP_EXTMAP_VIDEO_ORIENTATION);
					janus_sdp_attribute_add_to_mline(janus_sdp_mline_find(answer, JANUS_SDP_VIDEO), a);
				}
				if(playout_delay_extmap) {
					/* First of all, let's check if the extmap attribute had a direction */
					const char *direction = NULL;
					switch(playout_delay_mdir) {
						case JANUS_SDP_SENDONLY:
							direction = "/recvonly";
							break;
						case JANUS_SDP_RECVONLY:
						case JANUS_SDP_INACTIVE:
							direction = "/inactive";
							break;
						default:
							direction = "";
							break;
					}
					janus_sdp_attribute *a = janus_sdp_attribute_create("extmap",
						"%d%s %s\r\n", participant->playout_delay_extmap_id, direction, JANUS_RTP_EXTMAP_PLAYOUT_DELAY);
					janus_sdp_attribute_add_to_mline(janus_sdp_mline_find(answer, JANUS_SDP_VIDEO), a);
				}
				if (transport_wide_cc_extmap) {
					janus_sdp_attribute *a = janus_sdp_attribute_create("extmap",
						"%d %s\r\n", participant->transport_wide_cc_extmap_id, JANUS_RTP_EXTMAP_TRANSPORT_WIDE_CC);
					janus_sdp_attribute_add_to_mline(janus_sdp_mline_find(answer, JANUS_SDP_VIDEO), a);
				}
				/* Generate an SDP string we can send back to the publisher */
				char *answer_sdp = janus_sdp_write(answer);
				/* Now turn the SDP into what we'll send subscribers, using the static payload types for making switching easier */
				offer = janus_sdp_generate_offer(s_name, answer->c_addr,
					JANUS_SDP_OA_AUDIO, participant->audio,
					JANUS_SDP_OA_AUDIO_CODEC, janus_videoroom_audiocodec_name(participant->acodec),
					JANUS_SDP_OA_AUDIO_PT, janus_videoroom_audiocodec_pt(participant->acodec),
					JANUS_SDP_OA_AUDIO_DIRECTION, JANUS_SDP_SENDONLY,
					JANUS_SDP_OA_VIDEO, participant->video,
					JANUS_SDP_OA_VIDEO_CODEC, janus_videoroom_videocodec_name(participant->vcodec),
					JANUS_SDP_OA_VIDEO_PT, janus_videoroom_videocodec_pt(participant->vcodec),
					JANUS_SDP_OA_VIDEO_DIRECTION, JANUS_SDP_SENDONLY,
					JANUS_SDP_OA_DATA, participant->data,
					JANUS_SDP_OA_DONE);
				/* Add the extmap attributes, if needed */
				if(audio_level_extmap) {
					janus_sdp_mline *m = janus_sdp_mline_find(offer, JANUS_SDP_AUDIO);
					if(m != NULL) {
						janus_sdp_attribute *a = janus_sdp_attribute_create("extmap",
							"%d %s\r\n", participant->audio_level_extmap_id, JANUS_RTP_EXTMAP_AUDIO_LEVEL);
						janus_sdp_attribute_add_to_mline(m, a);
					}
				}
				if(video_orient_extmap) {
					janus_sdp_mline *m = janus_sdp_mline_find(offer, JANUS_SDP_VIDEO);
					if(m != NULL) {
						janus_sdp_attribute *a = janus_sdp_attribute_create("extmap",
							"%d %s\r\n", participant->video_orient_extmap_id, JANUS_RTP_EXTMAP_VIDEO_ORIENTATION);
						janus_sdp_attribute_add_to_mline(m, a);
					}
				}
				if(playout_delay_extmap) {
					janus_sdp_mline *m = janus_sdp_mline_find(offer, JANUS_SDP_VIDEO);
					if(m != NULL) {
						janus_sdp_attribute *a = janus_sdp_attribute_create("extmap",
							"%d %s\r\n", participant->playout_delay_extmap_id, JANUS_RTP_EXTMAP_PLAYOUT_DELAY);
						janus_sdp_attribute_add_to_mline(m, a);
					}
				}
				/* DO not send transport wide CC to subscribers */
				/* Generate an SDP string we can offer subscribers later on */
				char *offer_sdp = janus_sdp_write(offer);
				if(!sdp_update) {
					/* Is this room recorded? */
					janus_mutex_lock(&participant->rec_mutex);
					if(videoroom->record || participant->recording_active) {
						janus_videoroom_recorder_create(participant, participant->audio, participant->video, participant->data);
					}
					/* Is simulcasting involved */
					if(msg_simulcast && participant->vcodec == JANUS_VIDEOROOM_VP8) {
						JANUS_LOG(LOG_VERB, "Publisher is going to do simulcasting\n");
						participant->ssrc[0] = json_integer_value(json_object_get(msg_simulcast, "ssrc-0"));
						participant->ssrc[1] = json_integer_value(json_object_get(msg_simulcast, "ssrc-1"));
						participant->ssrc[2] = json_integer_value(json_object_get(msg_simulcast, "ssrc-2"));
					} else {
						/* No simulcasting involved */
						participant->ssrc[0] = 0;
						participant->ssrc[1] = 0;
						participant->ssrc[2] = 0;
					}
					janus_mutex_unlock(&participant->rec_mutex);
				}
				janus_sdp_destroy(offer);
				janus_sdp_destroy(answer);
				/* Send the answer back to the publisher */
				JANUS_LOG(LOG_VERB, "Handling publisher: turned this into an '%s':\n%s\n", type, answer_sdp);
				json_t *jsep = json_pack("{ssss}", "type", type, "sdp", answer_sdp);
				g_free(answer_sdp);
				/* How long will the gateway take to push the event? */
				g_atomic_int_set(&session->hangingup, 0);
				gint64 start = janus_get_monotonic_time();
				int res = gateway->push_event(msg->handle, &janus_videoroom_plugin, msg->transaction, event, jsep);
				JANUS_LOG(LOG_VERB, "  >> Pushing event: %d (took %"SCNu64" us)\n", res, janus_get_monotonic_time()-start);
				/* Done */
				if(res != JANUS_OK) {
					/* TODO Failed to negotiate? We should remove this publisher */
					g_free(offer_sdp);
				} else {
					/* Store the participant's SDP for interested subscribers */
					g_free(participant->sdp);
					participant->sdp = offer_sdp;
					/* We'll wait for the setup_media event before actually telling subscribers */
				}
				/* Unless this is an update, in which case schedule a new offer for all viewers */
				if(sdp_update) {
					json_t *update = json_object();
					json_object_set_new(update, "request", json_string("configure"));
					json_object_set_new(update, "update", json_true());
					janus_mutex_lock(&participant->subscribers_mutex);
					GSList *s = participant->subscribers;
					while(s) {
						janus_videoroom_subscriber *subscriber = (janus_videoroom_subscriber *)s->data;
						if(subscriber && subscriber->session && subscriber->session->handle) {
							/* Enqueue the fake request: this will trigger a renegotiation */
							janus_videoroom_message *msg = g_malloc(sizeof(janus_videoroom_message));
							janus_refcount_increase(&subscriber->session->ref);
							msg->handle = subscriber->session->handle;
							msg->message = update;
							msg->transaction = NULL;
							msg->jsep = NULL;
							json_incref(update);
							g_async_queue_push(messages, msg);
						}
						s = s->next;
					}
					janus_mutex_unlock(&participant->subscribers_mutex);
					json_decref(update);
				}
				json_decref(event);
				json_decref(jsep);
			}
			if(participant != NULL)
				janus_refcount_decrease(&participant->ref);
		}
		janus_videoroom_message_free(msg);

		continue;
		
error:
		{
			/* Prepare JSON error event */
			json_t *event = json_object();
			json_object_set_new(event, "videoroom", json_string("event"));
			json_object_set_new(event, "error_code", json_integer(error_code));
			json_object_set_new(event, "error", json_string(error_cause));
			int ret = gateway->push_event(msg->handle, &janus_videoroom_plugin, msg->transaction, event, NULL);
			JANUS_LOG(LOG_VERB, "  >> Pushing event: %d (%s)\n", ret, janus_get_api_error(ret));
			json_decref(event);
			janus_videoroom_message_free(msg);
		}
	}
	JANUS_LOG(LOG_VERB, "Leaving VideoRoom handler thread\n");
	return NULL;
}

/* Helper to quickly relay RTP packets from publishers to subscribers */
static void janus_videoroom_relay_rtp_packet(gpointer data, gpointer user_data) {
	janus_videoroom_rtp_relay_packet *packet = (janus_videoroom_rtp_relay_packet *)user_data;
	if(!packet || !packet->data || packet->length < 1) {
		JANUS_LOG(LOG_ERR, "Invalid packet...\n");
		return;
	}
	janus_videoroom_subscriber *subscriber = (janus_videoroom_subscriber *)data;
	if(!subscriber || !subscriber->session) {
		// JANUS_LOG(LOG_ERR, "Invalid session...\n");
		return;
	}
	if(subscriber->paused || subscriber->kicked) {
		// JANUS_LOG(LOG_ERR, "This subscriber paused the stream...\n");
		return;
	}
	janus_videoroom_session *session = subscriber->session;
	if(!session || !session->handle) {
		// JANUS_LOG(LOG_ERR, "Invalid session...\n");
		return;
	}
	if(!session->started) {
		// JANUS_LOG(LOG_ERR, "Streaming not started yet for this session...\n");
		return;
	}
	
	/* Make sure there hasn't been a publisher switch by checking the SSRC */
	if(packet->is_video) {
		/* Check if this subscriber is subscribed to this medium */
		if(!subscriber->video) {
			/* Nope, don't relay */
			return;
		}
		/* Check if there's any SVC info to take into account */
		if(packet->svc) {
			/* There is: check if this is a layer that can be dropped for this viewer
			 * Note: Following core inspired by the excellent job done by Sergio Garcia Murillo here:
			 * https://github.com/medooze/media-server/blob/master/src/vp9/VP9LayerSelector.cpp */
			gboolean override_mark_bit = FALSE, has_marker_bit = packet->data->markerbit;
			int temporal_layer = subscriber->temporal_layer;
			if(subscriber->target_temporal_layer > subscriber->temporal_layer) {
				/* We need to upscale */
				JANUS_LOG(LOG_HUGE, "We need to upscale temporally:\n");
				if(packet->ubit && packet->bbit && packet->temporal_layer <= subscriber->target_temporal_layer) {
					JANUS_LOG(LOG_HUGE, "  -- Upscaling temporal layer: %u --> %u\n",
						packet->temporal_layer, subscriber->target_temporal_layer);
					subscriber->temporal_layer = packet->temporal_layer;
					temporal_layer = subscriber->temporal_layer;
					/* Notify the viewer */
					json_t *event = json_object();
					json_object_set_new(event, "videoroom", json_string("event"));
					json_object_set_new(event, "room", json_integer(subscriber->room_id));
					json_object_set_new(event, "temporal_layer", json_integer(subscriber->temporal_layer));
					gateway->push_event(subscriber->session->handle, &janus_videoroom_plugin, NULL, event, NULL);
					json_decref(event);
				}
			} else if(subscriber->target_temporal_layer < subscriber->temporal_layer) {
				/* We need to downscale */
				JANUS_LOG(LOG_HUGE, "We need to downscale temporally:\n");
				if(packet->ebit) {
					JANUS_LOG(LOG_HUGE, "  -- Downscaling temporal layer: %u --> %u\n",
						subscriber->temporal_layer, subscriber->target_temporal_layer);
					subscriber->temporal_layer = subscriber->target_temporal_layer;
					/* Notify the viewer */
					json_t *event = json_object();
					json_object_set_new(event, "videoroom", json_string("event"));
					json_object_set_new(event, "room", json_integer(subscriber->room_id));
					json_object_set_new(event, "temporal_layer", json_integer(subscriber->temporal_layer));
					gateway->push_event(subscriber->session->handle, &janus_videoroom_plugin, NULL, event, NULL);
					json_decref(event);
				}
			}
			if(temporal_layer < packet->temporal_layer) {
				/* Drop the packet: update the context to make sure sequence number is increased normally later */
				JANUS_LOG(LOG_HUGE, "Dropping packet (temporal layer %d < %d)\n", temporal_layer, packet->temporal_layer);
				subscriber->context.v_base_seq++;
				return;
			}
			int spatial_layer = subscriber->spatial_layer;
			if(subscriber->target_spatial_layer > subscriber->spatial_layer) {
				JANUS_LOG(LOG_HUGE, "We need to upscale spatially:\n");
				/* We need to upscale */
				if(packet->pbit == 0 && packet->bbit && packet->spatial_layer == subscriber->spatial_layer+1) {
					JANUS_LOG(LOG_HUGE, "  -- Upscaling spatial layer: %u --> %u\n",
						packet->spatial_layer, subscriber->target_spatial_layer);
					subscriber->spatial_layer = packet->spatial_layer;
					spatial_layer = subscriber->spatial_layer;
					/* Notify the viewer */
					json_t *event = json_object();
					json_object_set_new(event, "videoroom", json_string("event"));
					json_object_set_new(event, "room", json_integer(subscriber->room_id));
					json_object_set_new(event, "spatial_layer", json_integer(subscriber->spatial_layer));
					gateway->push_event(subscriber->session->handle, &janus_videoroom_plugin, NULL, event, NULL);
					json_decref(event);
				}
			} else if(subscriber->target_spatial_layer < subscriber->spatial_layer) {
				/* We need to downscale */
				JANUS_LOG(LOG_HUGE, "We need to downscale spatially:\n");
				if(packet->ebit) {
					JANUS_LOG(LOG_HUGE, "  -- Downscaling spatial layer: %u --> %u\n",
						subscriber->spatial_layer, subscriber->target_spatial_layer);
					subscriber->spatial_layer = subscriber->target_spatial_layer;
					/* Notify the viewer */
					json_t *event = json_object();
					json_object_set_new(event, "videoroom", json_string("event"));
					json_object_set_new(event, "room", json_integer(subscriber->room_id));
					json_object_set_new(event, "spatial_layer", json_integer(subscriber->spatial_layer));
					gateway->push_event(subscriber->session->handle, &janus_videoroom_plugin, NULL, event, NULL);
					json_decref(event);
				}
			}
			if(spatial_layer < packet->spatial_layer) {
				/* Drop the packet: update the context to make sure sequence number is increased normally later */
				JANUS_LOG(LOG_HUGE, "Dropping packet (spatial layer %d < %d)\n", spatial_layer, packet->spatial_layer);
				subscriber->context.v_base_seq++;
				return;
			} else if(packet->ebit && spatial_layer == packet->spatial_layer) {
				/* If we stop at layer 0, we need a marker bit now, as the one from layer 1 will not be received */
				override_mark_bit = TRUE;
			}
			/* If we got here, we can send the frame: this doesn't necessarily mean it's
			 * one of the layers the user wants, as there may be dependencies involved */
			JANUS_LOG(LOG_HUGE, "Sending packet (spatial=%d, temporal=%d)\n",
				packet->spatial_layer, packet->temporal_layer);
			/* Fix sequence number and timestamp (publisher switching may be involved) */
			janus_rtp_header_update(packet->data, &subscriber->context, TRUE, 4500);
			if(override_mark_bit && !has_marker_bit) {
				packet->data->markerbit = 1;
			}
			if(gateway != NULL)
				gateway->relay_rtp(session->handle, packet->is_video, (char *)packet->data, packet->length);
			if(override_mark_bit && !has_marker_bit) {
				packet->data->markerbit = 0;
			}
			/* Restore the timestamp and sequence number to what the publisher set them to */
			packet->data->timestamp = htonl(packet->timestamp);
			packet->data->seq_number = htons(packet->seq_number);
		} else if(packet->ssrc[0] != 0) {
			/* Handle simulcast: don't relay if it's not the SSRC we wanted to handle */
			uint32_t ssrc = ntohl(packet->data->ssrc);
			int plen = 0;
			char *payload = janus_rtp_payload((char *)packet->data, packet->length, &plen);
			if(payload == NULL)
				return;
			gboolean switched = FALSE;
			if(subscriber->substream != subscriber->substream_target) {
				/* There has been a change: let's wait for a keyframe on the target */
				int step = (subscriber->substream < 1 && subscriber->substream_target == 2);
				if(ssrc == packet->ssrc[subscriber->substream_target] || (step && ssrc == packet->ssrc[step])) {
					//~ if(janus_vp8_is_keyframe(payload, plen)) {
						uint32_t ssrc_old = 0;
						if(subscriber->substream != -1)
							ssrc_old = packet->ssrc[subscriber->substream];
						JANUS_LOG(LOG_VERB, "Received keyframe on SSRC %"SCNu32", switching (was %"SCNu32")\n", ssrc, ssrc_old);
						subscriber->substream = (ssrc == packet->ssrc[subscriber->substream_target] ? subscriber->substream_target : step);;
						switched = TRUE;
						/* Notify the viewer */
						json_t *event = json_object();
						json_object_set_new(event, "videoroom", json_string("event"));
						json_object_set_new(event, "room", json_integer(subscriber->room_id));
						json_object_set_new(event, "substream", json_integer(subscriber->substream));
						gateway->push_event(subscriber->session->handle, &janus_videoroom_plugin, NULL, event, NULL);
						json_decref(event);
					//~ } else {
						//~ JANUS_LOG(LOG_WARN, "Not a keyframe on SSRC %"SCNu32" yet, waiting before switching\n", ssrc);
					//~ }
				}
			}
			/* If we haven't received our desired substream yet, let's drop temporarily */
			if(subscriber->last_relayed == 0) {
				/* Let's start slow */
				subscriber->last_relayed = janus_get_monotonic_time();
			} else {
				/* Check if 250ms went by with no packet relayed */
				gint64 now = janus_get_monotonic_time();
				if(now-subscriber->last_relayed >= 250000) {
					subscriber->last_relayed = now;
					int substream = subscriber->substream-1;
					if(substream < 0)
						substream = 0;
					if(subscriber->substream != substream) {
						JANUS_LOG(LOG_WARN, "No packet received on substream %d for a while, falling back to %d\n",
							subscriber->substream, substream);
						subscriber->substream = substream;
						/* Send a PLI */
						JANUS_LOG(LOG_VERB, "Just (re-)enabled video, sending a PLI to recover it\n");
						char rtcpbuf[12];
						memset(rtcpbuf, 0, 12);
						janus_rtcp_pli((char *)&rtcpbuf, 12);
						if(subscriber->feed && subscriber->feed->session && subscriber->feed->session->handle) {
							gateway->relay_rtcp(subscriber->feed->session->handle, 1, rtcpbuf, 12);
							/* Update the time of when we last sent a keyframe request */
							subscriber->feed->fir_latest = janus_get_monotonic_time();
						}
						/* Notify the viewer */
						json_t *event = json_object();
						json_object_set_new(event, "videoroom", json_string("event"));
						json_object_set_new(event, "room", json_integer(subscriber->room_id));
						json_object_set_new(event, "substream", json_integer(subscriber->substream));
						gateway->push_event(subscriber->session->handle, &janus_videoroom_plugin, NULL, event, NULL);
						json_decref(event);
					}
				}
			}
			if(ssrc != packet->ssrc[subscriber->substream]) {
				JANUS_LOG(LOG_HUGE, "Dropping packet (it's from SSRC %"SCNu32", but we're only relaying SSRC %"SCNu32" now\n",
					ssrc, packet->ssrc[subscriber->substream]);
				return;
			}
			subscriber->last_relayed = janus_get_monotonic_time();
			/* Check if there's any temporal scalability to take into account */
			uint16_t picid = 0;
			uint8_t tlzi = 0;
			uint8_t tid = 0;
			uint8_t ybit = 0;
			uint8_t keyidx = 0;
			if(janus_vp8_parse_descriptor(payload, plen, &picid, &tlzi, &tid, &ybit, &keyidx) == 0) {
				//~ JANUS_LOG(LOG_WARN, "%"SCNu16", %u, %u, %u, %u\n", picid, tlzi, tid, ybit, keyidx);
				if(subscriber->templayer != subscriber->templayer_target) {
					/* FIXME We should be smarter in deciding when to switch */
					subscriber->templayer = subscriber->templayer_target;
					/* Notify the user */
					json_t *event = json_object();
					json_object_set_new(event, "videoroom", json_string("event"));
					json_object_set_new(event, "room", json_integer(subscriber->room_id));
					json_object_set_new(event, "temporal", json_integer(subscriber->templayer));
					gateway->push_event(subscriber->session->handle, &janus_videoroom_plugin, NULL, event, NULL);
					json_decref(event);
				}
				if(tid > subscriber->templayer) {
					JANUS_LOG(LOG_HUGE, "Dropping packet (it's temporal layer %d, but we're capping at %d)\n",
						tid, subscriber->templayer);
					/* We increase the base sequence number, or there will be gaps when delivering later */
					subscriber->context.v_base_seq++;
					return;
				}
			}
			/* If we got here, update the RTP header and send the packet */
			janus_rtp_header_update(packet->data, &subscriber->context, TRUE, 4500);
			char vp8pd[6];
			memcpy(vp8pd, payload, sizeof(vp8pd));
			janus_vp8_simulcast_descriptor_update(payload, plen, &subscriber->simulcast_context, switched);
			/* Send the packet */
			if(gateway != NULL)
				gateway->relay_rtp(session->handle, packet->is_video, (char *)packet->data, packet->length);
			/* Restore the timestamp and sequence number to what the publisher set them to */
			packet->data->timestamp = htonl(packet->timestamp);
			packet->data->seq_number = htons(packet->seq_number);
			/* Restore the original payload descriptor as well, as it will be needed by the next viewer */
			memcpy(payload, vp8pd, sizeof(vp8pd));
		} else {
			/* Fix sequence number and timestamp (publisher switching may be involved) */
			janus_rtp_header_update(packet->data, &subscriber->context, TRUE, 4500);
			/* Send the packet */
			if(gateway != NULL)
				gateway->relay_rtp(session->handle, packet->is_video, (char *)packet->data, packet->length);
			/* Restore the timestamp and sequence number to what the publisher set them to */
			packet->data->timestamp = htonl(packet->timestamp);
			packet->data->seq_number = htons(packet->seq_number);
		}
	} else {
		/* Check if this subscriber is subscribed to this medium */
		if(!subscriber->audio) {
			/* Nope, don't relay */
			return;
		}
		/* Fix sequence number and timestamp (publisher switching may be involved) */
		janus_rtp_header_update(packet->data, &subscriber->context, FALSE, 960);
		/* Send the packet */
		if(gateway != NULL)
			gateway->relay_rtp(session->handle, packet->is_video, (char *)packet->data, packet->length);
		/* Restore the timestamp and sequence number to what the publisher set them to */
		packet->data->timestamp = htonl(packet->timestamp);
		packet->data->seq_number = htons(packet->seq_number);
	}

	return;
}

static void janus_videoroom_relay_data_packet(gpointer data, gpointer user_data) {
	char *text = (char *)user_data;
	janus_videoroom_subscriber *subscriber = (janus_videoroom_subscriber *)data;
	if(!subscriber || !subscriber->session || !subscriber->data || subscriber->paused) {
		return;
	}
	janus_videoroom_session *session = subscriber->session;
	if(!session || !session->handle) {
		return;
	}
	if(!session->started) {
		return;
	}
	if(gateway != NULL && text != NULL) {
		JANUS_LOG(LOG_VERB, "Forwarding DataChannel message (%zu bytes) to viewer: %s\n", strlen(text), text);
		gateway->relay_data(session->handle, text, strlen(text));
	}
	return;
}
