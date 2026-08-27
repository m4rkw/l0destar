<!DOCTYPE html>
<html>
  <head>
    <title>Tracking — register passkey</title>
    <meta name="viewport" content="initial-scale=1.0">
    <meta charset="utf-8">
    <link rel="stylesheet" type="text/css" href="/static/css/style.css" />
    <script src="/static/js/jquery.min.js" type="text/javascript"></script>
    <script src="/static/js/register.js" type="text/javascript"></script>
  </head>
  <body>
    <div class="registration-form">
      <form method="post" action="#">
        <table>
          <tr>
            <td>Username:</td>
            <td><input type="text" name="username" value="{{ username }}" readonly /></td>
          </tr>
          <tr>
            <td></td>
            <td><input type="submit" name="register" value="register" disabled /></td>
          </tr>
          <tr>
            <td></td>
            <td><span id="message" style="display:none"></span></td>
          </tr>
          <tr>
            <td></td>
            <td><a id="continue" style="display:none" href="/login">continue to login</a></td>
          </tr>
        </table>
      </form>
    </div>
  </body>
</html>
