function arrayBufferToBase64(buffer) {
  const bytes = new Uint8Array(buffer);
  let binary = '';
  for (let i = 0; i < bytes.byteLength; i++) {
    binary += String.fromCharCode(bytes[i]);
  }
  return window.btoa(binary);
}

function arrayToArrayBuffer(data) {
  const bytes = new Uint8Array(data.length);
  for (let i = 0; i < data.length; i++) {
    bytes[i] = data[i];
  }
  return bytes.buffer;
}

$(document).ready(function() {
  $('#username').focus();

  $(document).on('click', 'input[name="login"]', function(e) {
    e.preventDefault();
    beginAuth();
  });

  $(document).on('keydown', '#username', function(e) {
    if (e.key === 'Enter') {
      e.preventDefault();
      beginAuth();
    }
  });
});

function beginAuth() {
  const username = $('#username').val();
  if (!username) {
    $('#message').text('please enter your username').show();
    return;
  }
  $('#message').hide();

  $.ajax({
    type: 'POST',
    url: '/authoptions',
    contentType: 'application/json',
    data: JSON.stringify({ username: username }),
    dataType: 'json',
    success: function(resp) {
      if (resp.status === 'ok') {
        completeAuth(resp.authoptions);
      }
    },
    error: function(xhr) {
      const msg = (xhr.responseJSON && xhr.responseJSON.message) || 'authentication failed';
      $('#message').text(msg).show();
    },
  });
}

async function completeAuth(options) {
  const credentialRequest = {
    publicKey: {
      challenge: arrayToArrayBuffer(options.challenge),
      allowCredentials: (options.allow_credentials || []).map(cred => ({
        type: 'public-key',
        id: arrayToArrayBuffer(cred.id),
      })),
    },
  };

  let credential;
  try {
    credential = await navigator.credentials.get(credentialRequest);
  } catch (e) {
    $('#message').text('passkey authentication cancelled').show();
    return;
  }

  const authenticationData = {
    rawId: arrayBufferToBase64(credential.rawId),
    response: {
      authenticatorData: arrayBufferToBase64(credential.response.authenticatorData),
      clientDataJSON: arrayBufferToBase64(credential.response.clientDataJSON),
      signature: arrayBufferToBase64(credential.response.signature),
    },
    type: credential.type,
  };

  const resp = await fetch('/authenticate', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      authentication_data: authenticationData,
      user_id: credential.id,
    }),
  });
  const data = await resp.json();

  if (data.status === 'ok') {
    window.location.href = '/';
  } else {
    $('#message').text(data.message || 'authentication failed').show();
  }
}
