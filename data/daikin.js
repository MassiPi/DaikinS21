var upTimeInterval, startTime, ws;
function upTimeMsg(uptime){
	// calculate (and subtract) whole days
	var days = Math.floor(uptime / 86400);
	uptime -= days * 86400;

	// calculate (and subtract) whole hours
	var hours = Math.floor(uptime / 3600) % 24;
	uptime -= hours * 3600;

	// calculate (and subtract) whole minutes
	var minutes = Math.floor(uptime / 60) % 60;
	uptime -= minutes * 60;
	
	return String(days).padStart(2, '0') + "d:" + String(hours).padStart(2, '0') + "h:" + String(minutes).padStart(2, '0') + "m";
}

$( document ).ready(function() {
	//enabling tooltips
	$('[data-toggle="tooltip"]').tooltip()
	
	//recurring update of uptime
	upTimeInterval = setInterval(function(){ 
		console.log("Calculating uptime");
		$('#upTime').html(upTimeMsg(Math.floor(Date.now() / 1000) - startTime));
	}, 60000);
	
	//setting up config sliders
	$("#cfgPeriod").ionRangeSlider({
        skin: "modern",
        min: 1,
        max: 120,
		from: 5,
		postfix: " sec",
        onFinish: function (data) {
			// fired on pointer release
			console.log("Setting period to: "+data.from);
			var json_arr = {};
			json_arr["command"] = "config";
			json_arr['target'] = "period";
			json_arr["value"] = data.from;
			ws.send(JSON.stringify(json_arr));
		}
    });
	
	var cfgPeriod = $("#cfgPeriod").data("ionRangeSlider");	

	//toggles commands
	$('#httpSecurityToggle').change(function() {
		console.log("Sending httpSecurity config (" + $(this).prop('checked') + ")");
		var json_arr = {};
		json_arr["command"] = "config";
		json_arr["target"] = "httpEnable";
		json_arr["value"] = $(this).prop('checked');
		ws.send(JSON.stringify(json_arr));
	});
	$('#httpControlToggle').change(function() {
		console.log("Sending httpControl config (" + $(this).prop('checked') + ")");
		var json_arr = {};
		json_arr["command"] = "config";
		json_arr["target"] = "httpControlEnable";
		json_arr["value"] = $(this).prop('checked');
		ws.send(JSON.stringify(json_arr));
	});
	$('#mqttControlToggle').change(function() {
		console.log("Sending mqttControl config (" + $(this).prop('checked') + ")");
		var json_arr = {};
		json_arr["command"] = "config";
		json_arr["target"] = "mqttControlEnable";
		json_arr["value"] = $(this).prop('checked');
		ws.send(JSON.stringify(json_arr));
	});

	//data forms
	$("#httpSecurityForm").on('submit', function(e){
		e.preventDefault();
		console.log("Sending HTTP access info: Username: " + $("#httpUsername").val() + " and Password: --omitted--");
		var json_arr = {};
		json_arr["command"] = "config";
		json_arr["target"] = "httpAccessData";
		json_arr["username"] = $("#httpUsername").val();
		json_arr["password"] = $("#httpPassword").val();
		ws.send(JSON.stringify(json_arr));
	});
	$("#mqttSecurityForm").on('submit', function(e){
		e.preventDefault();
		console.log("Sending MQTT access info: Username: " + $("#mqttUsername").val() + " and Password: --omitted--");
		var json_arr = {};
		json_arr["command"] = "config";
		json_arr["target"] = "mqttAccessData";
		json_arr["username"] = $("#mqttUsername").val();
		json_arr["password"] = $("#mqttPassword").val();
		ws.send(JSON.stringify(json_arr));
	});
	$("#mqttDataForm").on('submit', function(e){
		e.preventDefault();
		console.log("Sending MQTT config data: Broker: " + $("#mqttBroker").val() + " - PubTopic: " + $("#mqttPubTopic").val() + " - SubTopic: " + $("#mqttSubTopic").val() + " - TestamentTopic: " + $("#mqttTestamentTopic").val());
		var json_arr = {};
		json_arr["command"] = "config";
		json_arr["target"] = "mqttData";
		json_arr["broker"] = $("#mqttBroker").val();
		json_arr["subTopic"] = $("#mqttSubTopic").val();
		json_arr["pubTopic"] = $("#mqttPubTopic").val();
		json_arr["testamentTopic"] = $("#mqttTestamentTopic").val();
		ws.send(JSON.stringify(json_arr));
	});
	$("#hostnameForm").on('submit', function(e){
		e.preventDefault();
		if ( confirm("Change hostname?") ) {
			console.log("Sending new hostname: " + $("#hostname").val());
			var json_arr = {};
			json_arr["command"] = "config";
			json_arr["target"] = "hostname";
			json_arr["value"] = $("#hostname").val();
			ws.send(JSON.stringify(json_arr));
		}
	});	
	// Bottons commands control
	$('#cmdRst').click(function(event) {
		event.preventDefault();
		if ( confirm("Reset device?") ) {
			console.log("Sending reset command");
			var json_arr = {};
			json_arr["command"] = "rstDevice";
			ws.send(JSON.stringify(json_arr));
			//refreshing page in 5 seconds
			setTimeout(function(){ location.reload(); }, 5000);
		} else {
			console.log("Reset command canceled");
		}
	});
	$('#cmdWifiRst').click(function(event) {
		event.preventDefault();
		if ( confirm("Reset wifi?") ) {
			console.log("Sending reset wifi command");
			var json_arr = {};
			json_arr["command"] = "rstWifi";
			ws.send(JSON.stringify(json_arr));
		} else {
			console.log("Reset wifi command canceled");
		}
	});


	//ac control
	//temp select
	$(".temp-select").on('click', function(e){
		//changing temp set, within limits 10-32
		$('.target-temp').text(Math.max(10, Math.min( 32, Number($('.target-temp').text().slice(0, -2).trim()) + Number($(this).data("delta")))) + "°C");
		
		//and sending
		var json_arr = {};
		json_arr["command"] = "acTemp";
		json_arr["temp"] = $('.target-temp').text().slice(0, -2).trim();

		ws.send(JSON.stringify(json_arr));	

		console.log("Sending temperature command for AC");
		console.log(JSON.stringify(json_arr));
	});

	//pwr-buttons
	$('#pwr-button').change(function(){
		console.log("Pwr switch changed: now " + $(this).is(':checked'));
		$("label[for='pwr-button']").removeClass("btn-danger btn-success");
		if($(this).prop("checked")){
			$("label[for='pwr-button']").addClass("btn-success");
		} else {
			$("label[for='pwr-button']").addClass("btn-danger");
		}
		var json_arr = {};
		json_arr["command"] = "acPower";
		json_arr["power"] = $('#pwr-button').prop("checked");

		ws.send(JSON.stringify(json_arr));	

		console.log("Sending power command for AC");
		console.log(JSON.stringify(json_arr));
	});
	
	//mode radio
	$("input[type='radio'][name='mode']").on('change', function(e){
		var json_arr = {};
		json_arr["command"] = "acMode";
		json_arr["mode"] = $("input[type='radio'][name='mode']:checked").val();

		ws.send(JSON.stringify(json_arr));	

		console.log("Sending mode command for AC");
		console.log(JSON.stringify(json_arr));
	});
	//fan radio
	$("input[type='radio'][name='fan']").on('change', function(e){
		var json_arr = {};
		json_arr["command"] = "acFan";
		json_arr["fan"] = $("input[type='radio'][name='fan']:checked").val();

		ws.send(JSON.stringify(json_arr));	

		console.log("Sending fan command for AC");
		console.log(JSON.stringify(json_arr));
	});
	
	//swingV
	$("#oscv-button").on('click', function(e){
		var json_arr = {};
		json_arr["command"] = "acSwingV";
		json_arr["swingV"] = $('#oscv-button').prop("checked");
		ws.send(JSON.stringify(json_arr));	

		console.log("Sending SwingV command for AC");
		console.log(JSON.stringify(json_arr));
	});
	//swingH
	$("#osch-button").on('click', function(e){
		var json_arr = {};
		json_arr["command"] = "acSwingH";
		json_arr["swingH"] = $('#osch-button').prop("checked");
		ws.send(JSON.stringify(json_arr));	

		console.log("Sending SwingH command for AC");
		console.log(JSON.stringify(json_arr));
	});

	// Let us open a web socket, not secure
	ws = new WebSocket("ws://" + window.location.host + "/ws");
	ws.onopen = function() {
		console.log("WebSocket connected.")
	};
	
	ws.onmessage = function (evt) { 
		var received_msg = evt.data;
		console.log("Received: " + received_msg);
		try{
			var data = $.parseJSON(received_msg);
			if(data["type"] == "sensor"){

				//temp
				$('.target-temp').text( Number(data['setpoint']/10.0) + "°C");
				//power
				$('#pwr-button').prop("checked", data["power"]);
				if ( data["power"] == true ) $("label[for='pwr-button']").removeClass('btn-danger').addClass('btn-success');
				else $("label[for='pwr-button']").removeClass('btn-success').addClass('btn-danger');
				//swings
				$('#oscv-button').prop("checked", data["swing_v"]);
				$('#osch-button').prop("checked", data["swing_h"]);
				//radios
				//fix auto mode that has double value
				if ( data["mode"] == 48 ) data["mode"] = 49;
				$("input[type='radio'][name='mode'][value='" + data["mode"] + "']").prop("checked", "true");
				//if mode is DRY or FAN (50 or 54) disable temp buttons
				if ( data["mode"] == 50 || data["mode"] == 54 ){
					$('.temp-select').prop("disabled", true);
					$('.target-temp').text("--°C");
				} else {
					$('.temp-select').prop("disabled", false);
					$('.target-temp').text( Number(data['setpoint']/10.0) + "°C");
				}
				$("input[type='radio'][name='fan'][value='" + data["fan"] + "']").prop("checked", "true");

				$('#inTemp').text(data['temp_inside']/10.0 + "°C");
				$('#outTemp').text(data['temp_outside']/10.0 + "°C");
				$('#coilTemp').text(data['temp_coil']/10.0 + "°C");
				$('#fanSpeed').text(data['fan_rpm'] + " rpm");
				
				if ( data["idle"] == true ) $("#compressor").removeClass('btn-warning').addClass('btn-light');
				else $("#compressor").removeClass('btn-light').addClass('btn-warning');

				
			} else if (data["type"] == "config"){
				cfgPeriod.update({
					from: data["period"]
				});
				$('#hostname').val(data['hostname']);
				$('#subTitle').html(data['hostname']);
				
				//http access
				if ( data['httpSecurityEnable'] ) $('#httpSecurityToggle').bootstrapToggle('on', true);
				else $('#httpSecurityToggle').bootstrapToggle('off', true);
				$("#httpUsername").val(data['httpSecurityUser']);
				
				//http and mqtt control toggles
				if ( data['httpControlEnable'] ) $('#httpControlToggle').bootstrapToggle('on', true);
				else $('#httpControlToggle').bootstrapToggle('off', true);
				if ( data['mqttControlEnable'] ) $('#mqttControlToggle').bootstrapToggle('on', true);
				else $('#mqttControlToggle').bootstrapToggle('off', true);

				$("#mqttUsername").val(data['mqttUser']);

				//mqtt data
				$('#mqttBroker').val(data['mqttBroker']);
				$('#mqttSubTopic').val(data['mqttSubTopic']);
				$('#mqttPubTopic').val(data['mqttPubTopic']);
				$('#mqttTestamentTopic').val(data['mqttTestamentTopic']);
				
				//reset needed -> show alert
				if ( data['resetNeeded'] ){
					$('#restartNeededAlert').show();
				} else {
					$('#restartNeededAlert').hide();
				}

			} else if (data["type"] == "rssi"){
				$('#wifiRssi').html("(" + data['value'] + "dBm)");
			} else if ( data["type"] == "startTime" ){
				$('#startTimeMsg').html(new Date(data['startTime'] * 1000).toLocaleDateString("it-IT") + " " + new Date(data['startTime'] * 1000).toLocaleTimeString("it-IT"));
				//also calculating first uptime
				startTime = data['startTime'];
				$('#upTime').html(upTimeMsg(Math.floor(Date.now() / 1000) - startTime));
			} else if (data["type"] == "info"){
				$('#hostname').html(data['hostname']);
				$('#ipAddress').html(data['ipAddress']);
				$('#cpuMhz').html(data['cpuMhz'] + "MHz");
				$('#flashMhz').html(data['flashMhz'] + "MHz");
				$('#wifiNetwork').html(data['wifiNetwork']);
				$('#chipId').html(data['chipId']);
				$('#coreVer').html(data['coreVer']);
				$('#sdkVer').html(data['sdkVer']);
				$('#lastRst').html(data['lastRst']);
			}
		} catch(err) {
		  console.log(err.message);
		}
	};

	ws.onclose = function() { 
		// websocket is closed.
		clearInterval(upTimeInterval);
		console.log("Connection is closed...");
		$(".loader").fadeIn("fast", function() {
			if ( confirm("Websocket connection closed!\nReload page?") ) {
				location.reload();
			}
		});
	};

	setTimeout(function(){ $(".loader").fadeOut("slow"); }, 1000);
});