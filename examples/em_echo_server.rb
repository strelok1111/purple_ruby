#
#Example Usage:
#
#Start the echo daemon
#$ruby examples/em_echo_server.rb
#
#Send im:
#$ telnet 127.0.0.1 8002
# prpl-msn,user@hotmail.com,password 
#$ telnet 127.0.0.1 8002
# prpl-jabber,user@gmail.com,password
#


require 'rubygems'
require 'eventmachine'
require 'purple_ruby'

module EchoServer
  def receive_data data
    protocol, user, password = data.split(",").collect { |x| x.chomp.strip }
    PurpleRuby.login(protocol, user, password)
    close_connection
  end
end

Thread.new do
  PurpleRuby.prefs_path = '.'
  PurpleRuby.init false

  PurpleRuby.watch_incoming_im do |acc, sender, message|
    sender = sender[0...sender.index('/')] if sender.index('/') #discard anything after '/'
    puts "recv: #{acc.username}, #{sender}, #{message}"
    acc.send_im(sender, message)
  end

  PurpleRuby.watch_signed_on_event do |acc|
    puts "signed on: #{acc.username}"
  end

  PurpleRuby.watch_connection_error do |acc, type, description|
    puts "connection_error: #{acc.username} #{type} #{description}"
    true #'true': auto-reconnect; 'false': do nothing
  end

  #request can be: 'SSL Certificate Verification' etc
  PurpleRuby.watch_request do |title, primary, secondary, who|
    puts "request: #{title}, #{primary}, #{secondary}, #{who}"
    true #'true': accept a request; 'false': ignore a request
  end

  #request for authorization when someone adds this account to their buddy list
  PurpleRuby.watch_new_buddy do |acc, remote_user, message|
    puts "new buddy: #{acc.username} #{remote_user} #{message}"
    true #'true': accept; 'false': deny
  end

  PurpleRuby.watch_notify_message do |type, title, primary, secondary|
    puts "notification: #{type}, #{title}, #{primary}, #{secondary}"
  end


  while true do
    sleep 0.1
    PurpleRuby.run_one_loop
  end
end

EM.run do
  EM.start_server "127.0.0.1", 8002, EchoServer
end