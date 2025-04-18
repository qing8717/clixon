#!/usr/bin/env bash
# List pagination tests according to draft-ietf-netconf-list-pagination-04
# Follow the example-social example in the draft and the tests in Appendix A.2 + A.3.1/A.3.2

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

APPNAME=example

cfg=$dir/conf.xml
fexample=$dir/example-social.yang
fstate=$dir/mystate.xml

# Set to true if LIST_PAGINATION_REMAINING is enabled
REMAINING=false

# Common example-module spec (fexample must be set)
. ./example_social.sh

# Define default restconfig config: RESTCONFIG
RESTCONFIG=$(restconf_config none false)
if [ $? -ne 0 ]; then
    err1 "Error when generating certs"
fi

# Validate internal state xml
: ${validatexml:=false}

cat <<EOF > $cfg
<clixon-config xmlns="http://clicon.org/config">
  <CLICON_CONFIGFILE>$cfg</CLICON_CONFIGFILE>
  <CLICON_FEATURE>ietf-netconf:startup</CLICON_FEATURE>
  <CLICON_FEATURE>clixon-restconf:allow-auth-none</CLICON_FEATURE> <!-- Use auth-type=none -->
  <CLICON_YANG_DIR>${YANG_INSTALLDIR}</CLICON_YANG_DIR>
  <CLICON_YANG_DIR>$IETFRFC</CLICON_YANG_DIR>
  <CLICON_YANG_MAIN_DIR>$dir</CLICON_YANG_MAIN_DIR>
  <CLICON_SOCK>/usr/local/var/run/$APPNAME.sock</CLICON_SOCK>
  <CLICON_BACKEND_DIR>/usr/local/lib/$APPNAME/backend</CLICON_BACKEND_DIR>
  <CLICON_BACKEND_PIDFILE>/usr/local/var/run/$APPNAME.pidfile</CLICON_BACKEND_PIDFILE>
  <CLICON_XMLDB_DIR>$dir</CLICON_XMLDB_DIR>
  <CLICON_XMLDB_FORMAT>json</CLICON_XMLDB_FORMAT>
  <CLICON_STREAM_DISCOVERY_RFC8040>true</CLICON_STREAM_DISCOVERY_RFC8040>
  <CLICON_CLI_MODE>$APPNAME</CLICON_CLI_MODE>
  <CLICON_CLI_DIR>/usr/local/lib/$APPNAME/cli</CLICON_CLI_DIR>
  <CLICON_CLISPEC_DIR>/usr/local/lib/$APPNAME/clispec</CLICON_CLISPEC_DIR>
  <CLICON_VALIDATE_STATE_XML>$validatexml</CLICON_VALIDATE_STATE_XML>
  $RESTCONFIG
</clixon-config>
EOF

# See draft-netconf-list-pagination-04.txt A.2 (except stats and audit-log)
# XXX: "config" without 
cat <<'EOF' > $dir/startup_db
{"config":
   {
     "example-social:members": {
       "member": [
         {
           "member-id": "alice",
           "email-address": "alice@example.com",
           "password": "$0$1543",
           "avatar": "BASE64VALUE=",
           "tagline": "Every day is a new day",
           "privacy-settings": {
             "hide-network": "false",
             "post-visibility": "public"
           },
           "following": ["bob", "eric", "lin"],
           "posts": {
             "post": [
               {
                 "timestamp": "2020-07-08T13:12:45Z",
                 "title": "My first post",
                 "body": "Hiya all!"
               },
               {
                 "timestamp": "2020-07-09T01:32:23Z",
                 "title": "Sleepy...",
                 "body": "Catch y'all tomorrow."
               }
             ]
           },
           "favorites": {
             "uint8-numbers": [17, 13, 11, 7, 5, 3],
             "int8-numbers": [-5, -3, -1, 1, 3, 5]
           }
         },
         {
           "member-id": "bob",
           "email-address": "bob@example.com",
           "password": "$0$1543",
           "avatar": "BASE64VALUE=",
           "tagline": "Here and now, like never before.",
           "posts": {
             "post": [
               {
                 "timestamp": "2020-08-14T03:32:25Z",
                 "body": "Just got in."
               },
               {
                 "timestamp": "2020-08-14T03:33:55Z",
                 "body": "What's new?"
               },
               {
                 "timestamp": "2020-08-14T03:34:30Z",
                 "body": "I'm bored..."
               }
             ]
           },
           "favorites": {
             "decimal64-numbers": ["3.14159", "2.71828"]
           }
         },
         {
           "member-id": "eric",
           "email-address": "eric@example.com",
           "password": "$0$1543",
           "avatar": "BASE64VALUE=",
           "tagline": "Go to bed with dreams; wake up with a purpose.",
           "following": ["alice"],
           "posts": {
             "post": [
               {
                 "timestamp": "2020-09-17T18:02:04Z",
                 "title": "Son, brother, husband, father",
                 "body": "What's your story?"
               }
             ]
           },
           "favorites": {
             "bits": ["two", "one", "zero"]
           }
         },
         {
           "member-id": "lin",
           "email-address": "lin@example.com",
           "password": "$0$1543",
           "privacy-settings": {
             "hide-network": "true",
             "post-visibility": "followers-only"
           },
           "following": ["joe", "eric", "alice"]
         },
         {
           "member-id": "joe",
           "email-address": "joe@example.com",
           "password": "$0$1543",
           "avatar": "BASE64VALUE=",
           "tagline": "Greatness is measured by courage and heart.",
           "privacy-settings": {
             "post-visibility": "unlisted"
           },
           "following": ["bob"],
           "posts": {
             "post": [
               {
                 "timestamp": "2020-10-17T18:02:04Z",
                 "body": "What's your status?"
               }
             ]
           }
         }
       ]
     }
   }
}
EOF

# See draft-netconf-list-pagination-04.txt A.2 (only stats and audit-log)
cat<<EOF > $fstate
<members xmlns="https://example.com/ns/example-social">
   <member>
      <member-id>alice</member-id>
      <stats>
          <joined>2020-07-08T12:38:32Z</joined>
          <membership-level>admin</membership-level>
          <last-activity>2021-04-01T02:51:11Z</last-activity>
      </stats>
   </member>
   <member>
      <member-id>bob</member-id>
      <stats>
          <joined>2020-08-14T03:30:00Z</joined>
          <membership-level>standard</membership-level>
          <last-activity>2020-08-14T03:34:30Z</last-activity>
      </stats>
   </member>
   <member>
      <member-id>eric</member-id>
      <stats>
          <joined>2020-09-17T19:38:32Z</joined>
          <membership-level>pro</membership-level>
          <last-activity>2020-09-17T18:02:04Z</last-activity>
      </stats>
   </member>
   <member>
      <member-id>lin</member-id>
      <stats>
          <joined>2020-07-09T12:38:32Z</joined>
          <membership-level>standard</membership-level>
          <last-activity>2021-04-01T02:51:11Z</last-activity>
      </stats>
   </member>
   <member>
      <member-id>joe</member-id>
      <stats>
          <joined>2020-10-08T12:38:32Z</joined>
          <membership-level>pro</membership-level>
          <last-activity>2021-04-01T02:51:11Z</last-activity>
      </stats>
   </member>
</members>
<audit-logs xmlns="https://example.com/ns/example-social">
   <audit-log>
      <timestamp>": "2020-10-11T06:47:59Z",</timestamp>
           <member-id>alice</member-id>
           <source-ip>192.168.0.92</source-ip>
           <request>POST /groups/group/2043</request>
           <outcome>true</outcome>
   </audit-log>
   <audit-log>
           <timestamp>2020-11-01T15:22:01Z</timestamp>
           <member-id>bob</member-id>
           <source-ip>192.168.2.16</source-ip>
           <request>POST /groups/group/123</request>
           <outcome>false</outcome>
   </audit-log>
   <audit-log>
           <timestamp>2020-12-12T21:00:28Z</timestamp>
           <member-id>eric</member-id>
           <source-ip>192.168.254.1</source-ip>
           <request>POST /groups/group/10</request>
           <outcome>true</outcome>
   </audit-log>
   <audit-log>
           <timestamp>2021-01-03T06:47:59Z</timestamp>
           <member-id>alice</member-id>
           <source-ip>192.168.0.92</source-ip>
           <request>POST /groups/group/333</request>
           <outcome>true</outcome>
   </audit-log>
   <audit-log>
           <timestamp>2021-01-21T10:00:00Z</timestamp>
           <member-id>bob</member-id>
           <source-ip>192.168.2.16</source-ip>
           <request>POST /groups/group/42</request>
           <outcome>true</outcome>
   </audit-log>
   <audit-log>
           <timestamp>2020-02-07T09:06:21Z</timestamp>
           <member-id>alice</member-id>
           <source-ip>192.168.0.92</source-ip>
           <request>POST /groups/group/1202</request>
           <outcome>true</outcome>
   </audit-log>
   <audit-log>
           <timestamp>2020-02-28T02:48:11Z</timestamp>
           <member-id>bob</member-id>
           <source-ip>192.168.2.16</source-ip>
           <request>POST /groups/group/345</request>
           <outcome>true</outcome>
   </audit-log>
</audit-logs>
EOF

# Run limit-only test with netconf, restconf+xml and restconf+json
# Args:
# 1. offset
# 2. limit
# 3. remaining
# 4. list
function testlimit()
{
    offset=$1
    limit=$2
    remaining=$3
    list=$4

    # "clixon get"
    xmllist=""  # for netconf
    xmllist2="" # for restconf xml
    jsonlist="" # for restconf json
    jsonmeta=""
    let i=0

    for li in $list; do
        if [ $i = 0 ]; then
            # Note: if REMAINING is enabled:
            #       if [ $limit == 0 ]; then
            if ${REMAINING}; then
                el="<uint8-numbers lp:remaining=\"$remaining\" xmlns:lp=\"urn:ietf:params:xml:ns:yang:ietf-list-pagination\">$li</uint8-numbers>"
                el2="<uint8-numbers lp:remaining=\"$remaining\" xmlns:lp=\"urn:ietf:params:xml:ns:yang:ietf-list-pagination\" xmlns=\"https://example.com/ns/example-social\">$li</uint8-numbers>"
                jsonmeta=",\"@example-social:uint8-numbers\":\[{\"ietf-list-pagination:remaining\":$remaining}\]"
            else
                el="<uint8-numbers>$li</uint8-numbers>"
                el2="<uint8-numbers xmlns=\"https://example.com/ns/example-social\">$li</uint8-numbers>"
            fi
            jsonlist="$li"
        else
            el="<uint8-numbers>$li</uint8-numbers>"
            el2="<uint8-numbers xmlns=\"https://example.com/ns/example-social\">$li</uint8-numbers>"            jsonlist="$jsonlist,$li"
        fi
        xmllist="$xmllist$el"
        xmllist2="$xmllist2$el2"
        let i++
    done

    jsonstr=""
    if [ $limit -eq 0 ]; then
        limitxmlstr=""
    else
        limitxmlstr="<limit>$limit</limit>"
        jsonstr="?limit=$limit"
    fi
    if [ $offset -eq 0 ]; then
        offsetxmlstr=""
    else
        offsetxmlstr="<offset>$offset</offset>"
        if [ -z "$jsonstr" ]; then
            jsonstr="?offset=$offset"
        else
            jsonstr="${jsonstr}&offset=$offset"
        fi
    fi

    if [ -z "$list" ]; then
        reply="<rpc-reply $DEFAULTNS><data/></rpc-reply>"
    else
        reply="<rpc-reply $DEFAULTNS><data><members xmlns=\"https://example.com/ns/example-social\"><member><member-id>alice</member-id><favorites>$xmllist</favorites></member></members></data></rpc-reply>"
    fi
    new "limit=$limit offset=$offset NETCONF get-config"
    expecteof_netconf "$clixon_netconf -qf $cfg" 0 "$DEFAULTHELLO" "<rpc $DEFAULTNS><get-config><source><running/></source><filter type=\"xpath\" select=\"/es:members/es:member[es:member-id='alice']/es:favorites/es:uint8-numbers\" xmlns:es=\"https://example.com/ns/example-social\"/><list-pagination xmlns=\"urn:ietf:params:xml:ns:yang:ietf-list-pagination-nc\">$limitxmlstr$offsetxmlstr</list-pagination></get-config></rpc>" "" "$reply"

    if [ -z "$list" ]; then
        reply="<rpc-reply $DEFAULTNS><data/></rpc-reply>"
    else
        reply="<rpc-reply $DEFAULTNS><data><members xmlns=\"https://example.com/ns/example-social\"><member><member-id>alice</member-id><favorites>$xmllist</favorites></member></members></data></rpc-reply>"
    fi
    new "limit=$limit offset=$offset NETCONF get"
    expecteof_netconf "$clixon_netconf -qf $cfg" 0 "$DEFAULTHELLO" "<rpc $DEFAULTNS><get><filter type=\"xpath\" select=\"/es:members/es:member[es:member-id='alice']/es:favorites/es:uint8-numbers\" xmlns:es=\"https://example.com/ns/example-social\"/><list-pagination xmlns=\"urn:ietf:params:xml:ns:yang:ietf-list-pagination-nc\">$limitxmlstr$offsetxmlstr</list-pagination></get></rpc>" "" "$reply"

    if [ -z "$list" ]; then
        reply="<xml-list xmlns=\"urn:ietf:params:xml:ns:yang:ietf-list-pagination\"/>"
    else
        reply="<xml-list xmlns=\"urn:ietf:params:xml:ns:yang:ietf-list-pagination\">$xmllist2</xml-list>"
    fi
    new "limit=$limit offset=$offset Parameter RESTCONF xml"
    expectpart "$(curl $CURLOPTS -X GET -H "Accept: application/yang-data+xml-list" $RCPROTO://localhost/restconf/data/example-social:members/member=alice/favorites/uint8-numbers${jsonstr})" 0 "HTTP/$HVER 200" "Content-Type: application/yang-data+xml-list" "$reply"
    if [ -z "$list" ]; then
        #       reply="{\"xml-list\":{}}"
        reply="{}"
    else
        reply="{\"example-social:uint8-numbers\":\[$jsonlist\]}"
    fi
    new "limit=$limit offset=$offset Parameter RESTCONF json"
    expectpart "$(curl $CURLOPTS -X GET -H "Accept: application/yang-data+json" $RCPROTO://localhost/restconf/data/example-social:members/member=alice/favorites/uint8-numbers${jsonstr})" 0 "HTTP/$HVER 200" "Content-Type: application/yang-data+json" "$reply" --not-- "xml-list"
} # testlimit

new "test params: -f $cfg -s startup -- -sS $fstate"

if [ $BE -ne 0 ]; then
    new "kill old backend"
    sudo clixon_backend -zf $cfg
    if [ $? -ne 0 ]; then
        err
    fi
    sudo pkill -f clixon_backend # to be sure

    new "start backend -s startup -f $cfg -- -sS $fstate"
    start_backend -s startup -f $cfg -- -sS $fstate
fi

new "wait backend"
wait_backend

if [ $RC -ne 0 ]; then
    new "kill old restconf daemon"
    stop_restconf_pre

    new "start restconf daemon"
    start_restconf -f $cfg
fi

new "wait restconf"
wait_restconf

new "Baseline: no pagination"
expecteof_netconf "$clixon_netconf -qf $cfg" 0 "$DEFAULTHELLO" "<rpc $DEFAULTNS><get><filter type=\"xpath\" select=\"/es:members/es:member[es:member-id='alice']/es:favorites/es:uint8-numbers\" xmlns:es=\"https://example.com/ns/example-social\"/></get></rpc>" "<rpc-reply $DEFAULTNS><data><members xmlns=\"https://example.com/ns/example-social\"><member><member-id>alice</member-id><favorite"

new "A.3.1.1. limit=1"
testlimit 0 1 5 "17"

new "A.3.1.2. limit=2"
testlimit 0 2 4 "17 13"

new "A.3.1.3. limit=5"
testlimit 0 5 1 "17 13 11 7 5"

new "A.3.1.4. limit=6"
testlimit 0 6 0 "17 13 11 7 5 3"

new "A.3.1.5. limit=7"
testlimit 0 7 0 "17 13 11 7 5 3"

new "A.3.2.1. offset=1"
testlimit 1 0 0 "13 11 7 5 3"

new "A.3.2.2. offset=2"
testlimit 2 0 0 "11 7 5 3"

new "A.3.2.3. offset=5"
testlimit 5 0 0 "3"

new "A.3.2.4. offset=6"
testlimit 6 0 0 ""

# This is incomplete wrt the draft
new "A.3.7. limit=2 offset=2"
testlimit 2 2 2 "11 7"

if [ $RC -ne 0 ]; then
    new "Kill restconf daemon"
    stop_restconf
fi

if [ $BE -ne 0 ]; then
    new "Kill backend"
    # Check if premature kill
    pid=$(pgrep -u root -f clixon_backend)
    if [ -z "$pid" ]; then
        err "backend already dead"
    fi
    # kill backend
    stop_backend -f $cfg
fi

unset validatexml

rm -rf $dir

new "endtest"
endtest
