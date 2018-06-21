Testtool
========

Testtool is the load balancing service of InnoGames.  It performs health
checks and manages PF and BGP accordingly.

Downtimes
---------

Downtimes are a feature useful in mass updates of servers.  They force
Testtool to mark given node in given pool as hard down, thus killing
all traffic to it and forbidding any new traffic to reach it.  When
a downtime for the given node ends, Testtool resumes health checks
against this node and only if all health checks pass, the node is marked
as up again and starts receiving traffic.

Health Checks
-------------

The following attributes are available for health checks:

* `hc_type`: Type of test to perform
* `hc_interval`: Interval to perform the checks
* `hc_max_failures`: Number of failed attempts to disable the LB node
* `hc_timeout`: Timeout in ms for a health check to be considered failed

Any other attributes are type-specific.

HTTP and HTTPS
--------------

The check sends the following HTTP request::

	$hc_query HTTP/1.1
	Host: $ip_addr
	Connection: close

The request is sent to the specified port, or port 80 for `http` or port
443 for `https`.  Health check port number has nothing to do with forwarded
(protocol_ports parameter) port!

Expected: HTTP answer with HTTP code

Fails on:

* Any answer other that is not real HTTP
* HTTP answer with unexpected code
* SSL connection issues (for https test)
* Connection timeout

Type-specific attributes:

* `hc_port`: Port number to connect to
* `hc_query`: HTTP method and the URL (path) to test
  (e.g. "HEAD /backend/lb_check.php")
* `hc_ok_codes`: List of HTTP codes treated as "OK"
* `hc_host`: Host header to be sent

Ping
----

ICMP Echo Request packets with icmp_id = pid of Testtool process and
increasing icmp_seq.  The icmp_seq number increases for each sent
packet and is stored for each tested node.  Received icmp_seq is used
to check which node the Echo Request was sent to with reverse checking.

Expected: ICMP Echo Reply with icmp_id as above

Fails on:

* ICMP Echo Reply with wrong icmp_id
* ICMP Echo Reply with proper icmp_id but icmp_seq not matching the last
  packet sent to given host is ignored
* Any other ICMP packet (so no DESTINATION_UNREACHABLE testing)

DNS
---

DNS query for a configured hostname with increasing transaction_id.  It
doesn't verify the contents of answer.

Expected: Any DNS reply with matching transaction_id and at least one answer
section

Fails on:

* Answer smaller than sizeof(dns_header)
* Answer bigger than DNS_BUFFER_SIZE
* No answer sections in answer
* DNS transaction_id different than one in the last sent query

Type-specific attributes:

* `hc_port`: Port number to connect to
* `hc_query`: Hostname to ask for

TCP
---

This check makes a TCP connection to the given port.

Expected: Full TCP handshake with remote service

Fails on:

* Remote port closed (reject)
* Timeout for establishing connection (drop)

Type-specific attributes:

* `hc_port`: Port to connect to on tested node

Postgres
--------

The check connects to Postgres database and executes a query.

Expected: A single row with a single column that is "true"

Fails on:

* Connection timeout
* Connection failure
* Authentication failure
* Any failure to run the given query
* Query returning no rows
* Query returning more than one rows
* Query return no columns
* Query returning more than one columns
* Anything else than "true" returned by the query

Type-specific attributes:

* `hc_port`: Port number to connect to
* `hc_user`: Username to connect the database server
* `hc_dbname`: Database name to connect
* `hc_query`: Query to execute on the database server

License
-------

The project is released under the MIT License.  The MIT License is registered
with and approved by the Open Source Initiative [1].

[1] https://opensource.org/licenses/MIT
